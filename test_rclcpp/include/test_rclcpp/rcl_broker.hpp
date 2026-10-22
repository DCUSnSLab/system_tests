// Copyright 2026 nev
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TEST_RCLCPP__RCL_BROKER_HPP_
#define TEST_RCLCPP__RCL_BROKER_HPP_

#include <cstdlib>
#include <cstring>

#include "rmw/rmw.h"

namespace test_rclcpp
{

// RCL_INTRA_PROCESS=1 makes rcl deliver messages between entities of one context through its
// broker, which reports from_intra_process. Only rmw_zenoh_cpp exports the broker API.
inline bool rcl_broker_enabled()
{
  const char * value = std::getenv("RCL_INTRA_PROCESS");
  return value && std::strcmp(value, "1") == 0 &&
         std::strcmp(rmw_get_implementation_identifier(), "rmw_zenoh_cpp") == 0;
}

}  // namespace test_rclcpp

#endif  // TEST_RCLCPP__RCL_BROKER_HPP_
