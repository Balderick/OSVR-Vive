/** @file
    @brief Implementation

    @date 2016

    @author
    Sensics, Inc.
    <http://sensics.com/osvr>
*/

// Copyright 2016 Sensics, Inc.
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

// Internal Includes
#include "OSVRViveTracker.h"
#include "DriverWrapper.h"
#include "GetComponent.h"
#include "PropertyHelper.h"

// Generated JSON header file
#include "com_osvr_Vive_json.h"

// Library/third-party includes
#include <osvr/Util/EigenCoreGeometry.h>
#include <osvr/Util/EigenInterop.h>

// Standard includes
// - none

namespace osvr {
namespace vive {

    namespace ei = osvr::util::eigen_interop;

    static const auto NUM_ANALOGS = 1;
    static const auto IPD_ANALOG = 0;

    static const auto HMD_SENSOR = 0;
    static const auto CONTROLLER_SENSORS = {1, 2};

    static const auto PREFIX = "[OSVR-Vive] ";

    ViveDriverHost::ViveDriverHost()
        : m_universeXform(Eigen::Isometry3d::Identity()),
          m_universeRotation(Eigen::Quaterniond::Identity()) {}

    bool ViveDriverHost::start(OSVR_PluginRegContext ctx,
                               osvr::vive::DriverWrapper &&inVive) {
        if (!inVive) {
            std::cerr << PREFIX << "Error: called ViveDriverHost::start() with "
                                   "an invalid vive object!"
                      << std::endl;
        }
        /// Take ownership of the Vive.
        m_vive.reset(new osvr::vive::DriverWrapper(std::move(inVive)));

        /// Finish setting up the Vive.
        if (!m_vive->startServerDeviceProvider()) {
            std::cerr << PREFIX << "Error: could not start the server "
                                   "device provider in the "
                                   "Vive driver. Exiting."
                      << std::endl;
            return false;
        }

        /// Power the system up.
        m_vive->serverDevProvider().LeaveStandby();

        auto handleNewDevice =
            [&](const char *serialNum) {
                auto dev = m_vive->serverDevProvider().FindTrackedDeviceDriver(
                    serialNum, vr::ITrackedDeviceServerDriver_Version);
                if (!dev) {
                    std::cout
                        << PREFIX
                        << "Couldn't find the corresponding device driver for "
                        << serialNum << std::endl;
                    return false;
                }
                auto ret = activateDevice(dev);
                if (!ret.first) {
                    std::cout << PREFIX << "Device with serial number "
                              << serialNum
                              << " couldn't be added to the devices vector."
                              << std::endl;
                    return false;
                }
                std::cout << "\n"
                          << PREFIX << "Device with s/n " << serialNum
                          << " activated, assigned ID " << ret.second
                          << std::endl;
                return true;
            };

        m_vive->driverHost().onTrackedDeviceAdded = handleNewDevice;

        /// Reserve ID 0 for the HMD
        m_vive->devices().reserveIds(1);

        {
            auto numDevices =
                m_vive->serverDevProvider().GetTrackedDeviceCount();
            std::cout << PREFIX << "Got " << numDevices
                      << " tracked devices at startup" << std::endl;
            for (decltype(numDevices) i = 0; i < numDevices; ++i) {
                auto dev = m_vive->serverDevProvider().GetTrackedDeviceDriver(
                    i, vr::ITrackedDeviceServerDriver_Version);
                activateDevice(dev);

#if 0
                addDeviceType(dev);
#endif
            }
        }

        /// Finish setting this up as an OSVR device.
        /// Create the initialization options
        OSVR_DeviceInitOptions opts = osvrDeviceCreateInitOptions(ctx);

        osvrDeviceTrackerConfigure(opts, &m_tracker);
        osvrDeviceAnalogConfigure(opts, &m_analog, NUM_ANALOGS);

        /// Because the callbacks may not come from the same thread that
        /// calls RunFrame, we need to be careful to not send directly from
        /// those callbacks. We can't use an Async device token because the
        /// waits are too long and they goof up the SteamVR Lighthouse driver.
        m_dev.initSync(ctx, "Vive", opts);

        /// Send JSON descriptor
        m_dev.sendJsonDescriptor(com_osvr_Vive_json);

        /// Register update callback
        m_dev.registerUpdateCallback(this);

        return true;
    }
    inline OSVR_ReturnCode ViveDriverHost::update() {
        m_vive->serverDevProvider().RunFrame();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            /// Copy a fixed number of tracking reports that have been
            /// queued up.
            auto numTrackers = m_trackingReports.size();
            for (std::size_t i = 0; i < numTrackers; ++i) {
                m_currentTrackingReports.push_back(m_trackingReports.front());
                m_trackingReports.pop_front();
            }

        } // unlock
        // Now that we're out of that mutex, we can go ahead and actually send
        // the reports.
        for (auto &out : m_currentTrackingReports) {
            if (out.isUniverseChange) {
                handleUniverseChange(out.newUniverse);
            } else {
                convertAndSendTracker(out.timestamp, out.sensor, out.report);
            }
        }
        // then clear this temporary buffer for next time.
        m_currentTrackingReports.clear();
        return OSVR_RETURN_SUCCESS;
    }

    std::pair<bool, std::uint32_t>
    ViveDriverHost::activateDevice(vr::ITrackedDeviceServerDriver *dev) {
        auto ret = activateDeviceImpl(dev);
        auto mfrProp = getStringProperty(dev, vr::Prop_ManufacturerName_String);
        auto modelProp = getStringProperty(dev, vr::Prop_ModelNumber_String);
        auto serialProp = getStringProperty(dev, vr::Prop_SerialNumber_String);
        std::cout << PREFIX;
        if (ret.first) {
            std::cout << "Assigned sensor ID " << ret.second << " to ";
        } else {
            std::cout << "Could not assign a sensor ID to ";
        }
        std::cout << mfrProp.first << " " << modelProp.first << " "
                  << serialProp.first << std::endl;
        return ret;
    }

    std::pair<bool, std::uint32_t>
    ViveDriverHost::activateDeviceImpl(vr::ITrackedDeviceServerDriver *dev) {
        auto &devs = m_vive->devices();
        if (getComponent<vr::IVRDisplayComponent>(dev)) {
            /// This is the HMD, since it has the display component.
            /// Always sensor 0.
            return devs.addAndActivateDeviceAt(dev, HMD_SENSOR);
        }
        if (getComponent<vr::IVRControllerComponent>(dev)) {
            /// This is a controller.
            for (auto ctrlIdx : CONTROLLER_SENSORS) {
                if (!devs.hasDeviceAt(ctrlIdx)) {
                    return devs.addAndActivateDeviceAt(dev, ctrlIdx);
                }
            }
        }
        /// This still may be a controller, if somehow there are more than
        /// 2...
        return devs.addAndActivateDevice(dev);
    }

    void ViveDriverHost::submitTrackingReport(uint32_t unWhichDevice,
                                              OSVR_TimeValue const &tv,
                                              const DriverPose_t &newPose) {
        TrackingReport out;
        out.timestamp = tv;
        out.sensor = unWhichDevice;
        out.report = newPose;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_trackingReports.push_back(std::move(out));
        }
    }

    void ViveDriverHost::submitUniverseChange(std::uint64_t newUniverse) {
        TrackingReport out;
        out.isUniverseChange = true;
        out.newUniverse = newUniverse;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_trackingReports.push_back(std::move(out));
        }
    }

    void ViveDriverHost::convertAndSendTracker(OSVR_TimeValue const &tv,
                                               OSVR_ChannelCount sensor,
                                               const DriverPose_t &newPose) {

        if (!newPose.poseIsValid) {
            /// @todo better handle non-valid states?
            return;
        }
        auto quatFromSteamVR = [](vr::HmdQuaternion_t const &q) {
            return Eigen::Quaterniond(q.w, q.x, q.y, q.z);
        };

        using namespace Eigen;

        auto qRotation = quatFromSteamVR(newPose.qRotation);

        auto driverFromHeadRotation =
            quatFromSteamVR(newPose.qDriverFromHeadRotation);

        Translation3d driverFromHeadTranslation(
            Vector3d::Map(newPose.vecDriverFromHeadTranslation));
        Isometry3d driverFromHead =
            driverFromHeadTranslation * driverFromHeadRotation;
#if 0
        driverFromHead.fromPositionOrientationScale(
            Vector3d::Map(newPose.vecDriverFromHeadTranslation),
            driverFromHeadRotation, Vector3d::Ones());
#endif
        auto worldFromDriverRotation =
            quatFromSteamVR(newPose.qWorldFromDriverRotation);
        Translation3d worldFromDriverTranslation(
            Vector3d::Map(newPose.vecWorldFromDriverTranslation));
        Isometry3d worldFromDriver =
            worldFromDriverTranslation * worldFromDriverRotation;
#if 0
        worldFromDriver.fromPositionOrientationScale(
            Eigen::Vector3d::Map(newPose.vecWorldFromDriverTranslation),
            worldFromDriverRotation, Eigen::Vector3d::Ones());
#endif
#if 0
        ei::map(pose.translation) =
            (worldFromDriverTranslation * worldFromDriverRotation *
                Eigen::Translation3d(Eigen::Vector3d::Map(newPose.vecPosition)) *
                driverFromHeadTranslation * worldFromDriverRotation)
            .translation();
#endif

        OSVR_Pose3 pose;
        ei::map(pose.translation) =
            (m_universeXform * worldFromDriver *
             Eigen::Translation3d(Eigen::Vector3d::Map(newPose.vecPosition)) *
             driverFromHeadTranslation)
                .translation();
        ei::map(pose.rotation) = m_universeRotation * worldFromDriverRotation *
                                 qRotation * driverFromHeadRotation;

        osvrDeviceTrackerSendPoseTimestamped(m_dev, m_tracker, &pose, sensor,
                                             &tv);
    }

    void ViveDriverHost::handleUniverseChange(std::uint64_t newUniverse) {
        /// Check to see if it's really a change
        if (newUniverse == m_universeId) {
            return;
        }
        std::cout << PREFIX << "Change of universe ID from " << m_universeId
                  << " to " << newUniverse << std::endl;
        m_universeId = newUniverse;
        auto known = m_vive->chaperone().knowUniverseId(m_universeId);
        if (!known) {
            std::cout << PREFIX << "No usable information on this universe "
                                   "could be found - there may not be a "
                                   "standing calibration for it in your room "
                                   "setup. You may wish to complete that then "
                                   "start the OSVR server again. Will operate "
                                   "without universe transforms."
                      << std::endl;
            m_universeXform.setIdentity();
            m_universeRotation.setIdentity();
        }

        /// Fetch the data
        auto univData = m_vive->chaperone().getDataForUniverse(m_universeId);
        using namespace Eigen;
        /// Populate the transforms.
        m_universeXform =
            Translation3d(Vector3d::Map(univData.translation.data())) *
            AngleAxisd(univData.yaw, Vector3d::UnitY());
        m_universeRotation =
            Quaterniond(AngleAxisd(univData.yaw, Vector3d::UnitY()));
    }

    void ViveDriverHost::TrackedDevicePoseUpdated(uint32_t unWhichDevice,
                                                  const DriverPose_t &newPose) {
        submitTrackingReport(unWhichDevice, osvr::util::time::getNow(),
                             newPose);
    }

    void ViveDriverHost::PhysicalIpdSet(uint32_t unWhichDevice,
                                        float fPhysicalIpdMeters) {
/// @todo - queue it up and send it from the main thread.
#if 0
        auto now = osvr::util::time::getNow();
        osvrDeviceAnalogSetValueTimestamped(m_dev, m_analog, fPhysicalIpdMeters,
            IPD_ANALOG, &now);
#endif
    }

    void ViveDriverHost::ProximitySensorState(uint32_t unWhichDevice,
                                              bool bProximitySensorTriggered) {
        /// @todo - queue it up and send it from the main thread.
    }

    void
    ViveDriverHost::TrackedDevicePropertiesChanged(uint32_t unWhichDevice) {
        if (HMD_SENSOR == unWhichDevice) {
            vr::ETrackedPropertyError err;
            auto universe = m_vive->devices()
                                .getDevice(HMD_SENSOR)
                                .GetUint64TrackedDeviceProperty(
                                    vr::Prop_CurrentUniverseId_Uint64, &err);
            if (vr::TrackedProp_Success != err) {
                /// set to invalid universe
                universe = 0;
            }
            /// Check our thread-local copy of the universe ID before submitting
            /// the message.
            if (m_trackingThreadUniverseId != universe) {
                m_trackingThreadUniverseId = universe;
                submitUniverseChange(universe);
            }
        }
    }

#if 0
    void
    ViveDriverHost::DeviceDescriptorUpdated(std::string const &json) {

        m_dev.sendJsonDescriptor(json);
    }
#endif

} // namespace vive
} // namespace osvr
