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
#include "FindDriver.h"
#include "DriverLoader.h"

// Library/third-party includes
#include <openvr_driver.h>
#include <osvr/Util/PlatformConfig.h>

// Standard includes
#include <cstdlib>
#include <iostream>
#include <vector>

#if _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace osvr {
namespace vive {

} // namespace vive
} // namespace osvr

int main() {
    auto driverLocation = osvr::vive::findDriver();
    if (driverLocation.found) {
        std::cout << "Found the Vive driver at " << driverLocation.driverFile
                  << std::endl;
    } else {
        std::cout << "Could not find the native SteamVR Vive driver, exiting!"
                  << std::endl;
        return 1;
    }

    osvr::vive::DriverLoader vive(driverLocation.driverRoot,
                                driverLocation.driverFile);
    return 0;
}
