/** @file
    @brief Header

    @date 2016

    @author
    Sensics, Inc.
    <http://sensics.com/osvr>
*/

// Copyright 2016 Razer Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef INCLUDED_DriverWrapper_h_GUID_F3D0F445_80E9_4C85_2CE4_E36293ED03F8
#define INCLUDED_DriverWrapper_h_GUID_F3D0F445_80E9_4C85_2CE4_E36293ED03F8

// Internal Includes
#include "ChaperoneData.h"
#include "DeviceHolder.h"
#include "DriverLoader.h"
#include "FindDriver.h"
#include "GetProvider.h"
#include "ServerDriverHost.h"

// Library/third-party includes
// - none

// Standard includes
#include <cstdint>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace osvr {
namespace vive {
    /// The do-nothing driver logger.
    class NullDriverLog : public vr::IDriverLog {
      public:
        void Log(const char *) override {}
    };

    class DriverWrapper {
      public:
        using DriverVector = std::vector<vr::ITrackedDeviceServerDriver *>;

        /// Constructor for when you have your own subclass of ServerDriverHost
        /// to pass in.
        explicit DriverWrapper(vr::ServerDriverHost *serverDriverHost)
            : serverDriverHost_(serverDriverHost),
              locations_(findLocationInfoForDriver()) {
            commonInit_();
        }

        /// Default constructor: we make and own our own ServerDriverHost.
        DriverWrapper()
            : owningServerDriverHost_(new vr::ServerDriverHost),
              serverDriverHost_(owningServerDriverHost_.get()),
              locations_(findLocationInfoForDriver()) {
            commonInit_();
        }

        ~DriverWrapper() { stop(); }

        DriverWrapper(DriverWrapper const &) = delete;
        DriverWrapper &operator=(DriverWrapper const &) = delete;

#if defined(_MSC_VER) && _MSC_VER < 1900
        /// Move constructor
        /// VS2013 doesn't know how to make a default move constructor :(
        DriverWrapper(DriverWrapper &&other)
            : owningServerDriverHost_(std::move(other.owningServerDriverHost_)),
              serverDriverHost_(std::move(other.serverDriverHost_)),
              locations_(std::move(other.locations_)),
              chaperone_(std::move(other.chaperone_)),
              loader_(std::move(other.loader_)),
              serverDeviceProvider_(std::move(other.serverDeviceProvider_)),
              devices_(std::move(other.devices_)) {}
#else
        /// Move constructor
        DriverWrapper(DriverWrapper &&other) = default;
#endif

        explicit operator bool() const {
            /// If you have the driver loaded and the config dirs found, that
            /// implies you found the driver.
            return foundConfigDirs() && haveDriverLoaded() &&
                   haveServerDeviceHost();
        }

        bool foundDriver() const { return locations_.driverFound; }

        std::string const &getDriverFileLocation() const {
            return locations_.driverFile;
        }

        bool foundConfigDirs() const { return locations_.configFound; }

        std::string const &getDriverConfigDir() const {
            return locations_.driverConfigDir;
        }

        std::string const &getRootConfigDir() const {
            return locations_.rootConfigDir;
        }

        bool haveDriverLoaded() const {
            // only one of these pointers will ever be initialized at a time,
            // since we move from loader_ to initialize serverDeviceProvider_

            // loader_ has an explicit operator bool to check its success, hence
            // the longer check.
            return (static_cast<bool>(loader_) &&
                    static_cast<bool>(*loader_)) ||
                   static_cast<bool>(serverDeviceProvider_);
        }

        bool haveServerDeviceHost() const {
            return static_cast<bool>(serverDriverHost_ != nullptr);
        }

        /// This method must be called before calling
        /// startServerDeviceProvider()
        bool isHMDPresent() {
            if (!(foundDriver() && foundConfigDirs() && haveDriverLoaded())) {
                return false;
            }
            if (!loader_) {
                throw std::logic_error("Calls to isHMDPresent must occur "
                                       "before startServerDeviceProvider!");
            }
            return loader_->isHMDPresent(locations_.rootConfigDir);
        }

        /// This must be called before accessing the server device provider.
        ///
        /// @param quiet If true (default), a "null" driver logger will be
        /// passed that discards all log data.
        ///
        /// @return false if it could not be started
        bool startServerDeviceProvider(bool quiet = true) {
            if (!(foundDriver() && foundConfigDirs() && haveDriverLoaded() &&
                  haveServerDeviceHost())) {
                return false;
            }
            if (serverDeviceProvider_) {
                return true;
            }
            vr::IDriverLog *logger = nullptr;
            if (quiet) {
                logger = &nullDriverLog_;
            }

            serverDeviceProvider_ =
                getProvider<vr::IServerTrackedDeviceProvider>(
                    std::move(loader_), logger, serverDriverHost_,
                    locations_.driverConfigDir);
            return static_cast<bool>(serverDeviceProvider_);
        }

        vr::IServerTrackedDeviceProvider &serverDevProvider() const {
            if (!haveDriverLoaded() || !serverDeviceProvider_) {
                throw std::logic_error("Attempted to access server device "
                                       "provider when driver loading or device "
                                       "provider initialization failed or was "
                                       "not yet completed!!");
            }
            return *serverDeviceProvider_;
        }

        vr::ServerDriverHost &driverHost() const { return *serverDriverHost_; }

        /// Access to the object that manages the array of device pointers,
        /// corresponding to the indices you assign them.
        DeviceHolder &devices() { return devices_; }

        /// @overload
        /// Const access
        DeviceHolder const &devices() const { return devices_; }

        bool haveChaperoneData() const { return static_cast<bool>(chaperone_); }
        ChaperoneData &chaperone() { return *chaperone_; }

        /// Set whether all devices should be deactivated on shutdown - defaults
        /// to true, so you might just want to set to false if, for instance,
        /// you deactivate and power off the devices on shutdown yourself.
        void disableDeactivateDevicesOnShutdown() {
            devices_.disableDeactivateOnShutdown();
        }

        /// Indicate to the system that you're preparing to stop the system.
        ///
        /// Sets the exiting flag on the server driver host, and if still
        /// enabled, deactivates all devices.
        void stop() {
            if (haveServerDeviceHost()) {
                serverDriverHost_->setExiting();
            }
            if (devices_.shouldDeactivateOnShutdown()) {
                devices_.deactivateAll();
            }
        }

      private:
        void commonInit_() {

            if (!foundDriver()) {
                return;
            }
            if (!foundConfigDirs()) {
                return;
            }
            chaperone_.reset(new ChaperoneData(getRootConfigDir()));

            loader_ = DriverLoader::make(locations_.driverRoot,
                                         locations_.driverFile);
            if (!haveDriverLoaded()) {
                return;
            }
        }
        /// This pointer manages lifetime if we created our own host but isn't
        /// accessed beyond that.
        std::unique_ptr<vr::ServerDriverHost> owningServerDriverHost_;
        /// This pointer is the one used, whether we create our own or get one
        /// passed in.
        vr::ServerDriverHost *serverDriverHost_;

        LocationInfo locations_;
        std::unique_ptr<ChaperoneData> chaperone_;

        std::unique_ptr<DriverLoader> loader_;
        ProviderPtr<vr::IServerTrackedDeviceProvider> serverDeviceProvider_;

        DeviceHolder devices_;
        NullDriverLog nullDriverLog_;
    };
} // namespace vive
} // namespace osvr

#endif // INCLUDED_DriverWrapper_h_GUID_F3D0F445_80E9_4C85_2CE4_E36293ED03F8
