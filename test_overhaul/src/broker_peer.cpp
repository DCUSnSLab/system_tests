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

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_components/register_node_macro.hpp"

#include "test_overhaul/payload.hpp"
#include "test_overhaul/srv/defer.hpp"

namespace test_overhaul
{

// The C++ side of the mixed container test: publishes cpp_to_py, checks py_to_cpp, and answers
// "deferred" requests on its next timer tick, twice each. It reports to $TEST_OVERHAUL_REPORT/cpp.
class BrokerPeer : public rclcpp::Node
{
public:
  explicit BrokerPeer(const rclcpp::NodeOptions & options)
  : Node("broker_peer", options),
    report_(std::string(std::getenv("TEST_OVERHAUL_REPORT")) + "/cpp")
  {
    publisher_ = create_publisher<msg::Payload>("cpp_to_py", 10);
    subscription_ = create_subscription<msg::Payload>(
      "py_to_cpp", 10,
      [this](msg::Payload::ConstSharedPtr message, const rclcpp::MessageInfo & info) {
        ++received_;
        intact_ += intact(*message, message->sequence);
        expected_route_ +=
        info.get_rmw_message_info().from_intra_process == rcl_broker_enabled();
      });
    service_ = create_service<srv::Defer>(
      "deferred",
      [this](std::shared_ptr<rmw_request_id_t> header, std::shared_ptr<srv::Defer::Request> request)
      {
        pending_.emplace_back(*header, request->value);
      });
    timer_ = create_wall_timer(std::chrono::milliseconds(50), [this]() {on_timer();});
  }

private:
  void on_timer()
  {
    auto message = std::make_unique<msg::Payload>();
    fill(*message, ++sequence_);
    publisher_->publish(std::move(message));
    for (auto & [header, value] : pending_) {
      srv::Defer::Response response;
      response.value = 2 * value;
      service_->send_response(header, response);
      service_->send_response(header, response);
      ++answered_twice_;
    }
    pending_.clear();
    const std::string partial = report_ + ".partial";
    std::ofstream(partial) << "received " << received_ << "\nintact " << intact_ <<
      "\nexpected_route " << expected_route_ << "\nanswered_twice " << answered_twice_ << "\n";
    std::rename(partial.c_str(), report_.c_str());
  }

  std::string report_;
  rclcpp::Publisher<msg::Payload>::SharedPtr publisher_;
  rclcpp::Subscription<msg::Payload>::SharedPtr subscription_;
  rclcpp::Service<srv::Defer>::SharedPtr service_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::vector<std::pair<rmw_request_id_t, int64_t>> pending_;
  uint64_t sequence_ = 0;
  int received_ = 0;
  int intact_ = 0;
  int expected_route_ = 0;
  int answered_twice_ = 0;
};

}  // namespace test_overhaul

RCLCPP_COMPONENTS_REGISTER_NODE(test_overhaul::BrokerPeer)
