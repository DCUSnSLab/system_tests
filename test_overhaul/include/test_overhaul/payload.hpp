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

#ifndef TEST_OVERHAUL__PAYLOAD_HPP_
#define TEST_OVERHAUL__PAYLOAD_HPP_

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "rmw/rmw.h"

#include "test_overhaul/msg/payload.hpp"

namespace test_overhaul
{

inline void fill(msg::Payload & message, uint64_t sequence)
{
  message.sequence = sequence;
  for (size_t i = 0; i < message.data.size(); ++i) {
    message.data[i] = static_cast<uint8_t>(sequence + i);
  }
}

inline bool intact(const msg::Payload & message, uint64_t sequence)
{
  if (message.sequence != sequence) {
    return false;
  }
  for (size_t i = 0; i < message.data.size(); ++i) {
    if (message.data[i] != static_cast<uint8_t>(sequence + i)) {
      return false;
    }
  }
  return true;
}

// RCL_INTRA_PROCESS=1 makes rcl deliver messages between entities of one context through its
// broker, which reports from_intra_process. Only rmw_zenoh_cpp exports the broker API.
inline bool rcl_broker_enabled()
{
  const char * value = std::getenv("RCL_INTRA_PROCESS");
  return value && std::strcmp(value, "1") == 0 &&
         std::strcmp(rmw_get_implementation_identifier(), "rmw_zenoh_cpp") == 0;
}

}  // namespace test_overhaul

#endif  // TEST_OVERHAUL__PAYLOAD_HPP_
