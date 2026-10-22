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

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "rclcpp/rclcpp.hpp"

#include "test_overhaul/payload.hpp"

using test_overhaul::msg::Payload;
using namespace std::chrono_literals;

// A subscription in another context receives through the rmw: the broker serves only its own
// context. It takes rmw loans unless RCL_INTRA_PROCESS=1, where rcl reports no subscription loans
// because broker deliveries are immutable owners.
// Messages kept past the callback stay intact while the publisher reuses its loans, and loaned
// delivery continues after the kept messages are released.
TEST(LoanLifetime, kept_messages_outlive_the_callback)
{
  rclcpp::init(0, nullptr);
  auto other = std::make_shared<rclcpp::Context>();
  other->init(0, nullptr);
  rclcpp::NodeOptions other_options;
  other_options.context(other);
  {
    auto node = std::make_shared<rclcpp::Node>("loan_publisher");
    auto remote = std::make_shared<rclcpp::Node>("loan_keeper", other_options);
    std::vector<std::shared_ptr<const Payload>> kept;
    size_t received = 0;
    auto subscription = remote->create_subscription<Payload>(
      "loan_lifetime", 4, [&](std::shared_ptr<const Payload> message) {
        if (message->sequence % 10 == 1) {
          kept.push_back(message);
        }
        ++received;
      });
    auto publisher = node->create_publisher<Payload>("loan_lifetime", 4);
    ASSERT_TRUE(publisher->can_loan_messages());
    ASSERT_EQ(!test_overhaul::rcl_broker_enabled(), subscription->can_loan_messages());
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = other;
    rclcpp::executors::SingleThreadedExecutor executor(executor_options);
    executor.add_node(remote);
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while ((publisher->get_subscription_count() == 0 ||
      subscription->get_publisher_count() == 0) && std::chrono::steady_clock::now() < deadline)
    {
      std::this_thread::sleep_for(10ms);
    }

    auto publish_and_receive = [&](uint64_t first, uint64_t last) {
        for (uint64_t sequence = first; sequence <= last; ++sequence) {
          auto loan = publisher->borrow_loaned_message();
          test_overhaul::fill(loan.get(), sequence);
          publisher->publish(std::move(loan));
          const auto until = std::chrono::steady_clock::now() + 5s;
          while (received < sequence && std::chrono::steady_clock::now() < until) {
            executor.spin_some(10ms);
          }
          ASSERT_EQ(sequence, received);
        }
      };
    publish_and_receive(1, 40);
    ASSERT_EQ(4u, kept.size());
    for (size_t i = 0; i < kept.size(); ++i) {
      EXPECT_TRUE(test_overhaul::intact(*kept[i], 10 * i + 1)) << "message " << 10 * i + 1;
    }
    kept.clear();
    publish_and_receive(41, 200);
    EXPECT_EQ(16u, kept.size());
    for (const auto & message : kept) {
      EXPECT_TRUE(test_overhaul::intact(*message, message->sequence));
    }
  }
  other->shutdown("test done");
  rclcpp::shutdown();
}
