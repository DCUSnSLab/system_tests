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
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "rclcpp/rclcpp.hpp"

#include "test_overhaul/payload.hpp"
#include "test_overhaul/srv/defer.hpp"

using test_overhaul::msg::Payload;
using test_overhaul::srv::Defer;
using namespace std::chrono_literals;

class Broker : public ::testing::Test
{
protected:
  void SetUp() override
  {
    rclcpp::init(0, nullptr);
  }

  void TearDown() override
  {
    rclcpp::shutdown();
  }

  void spin_until(rclcpp::Executor & executor, std::function<bool()> condition)
  {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
      executor.spin_some(10ms);
    }
  }
};

struct Received
{
  std::vector<uint64_t> sequences;
  int intact = 0;
  int intra_process = 0;
};

rclcpp::Subscription<Payload>::SharedPtr subscribe(
  rclcpp::Node & node, Received & received)
{
  return node.create_subscription<Payload>(
    "broker_payload", 20,
    [&received](Payload::ConstSharedPtr message, const rclcpp::MessageInfo & info) {
      received.sequences.push_back(message->sequence);
      received.intact += test_overhaul::intact(*message, message->sequence);
      received.intra_process += info.get_rmw_message_info().from_intra_process;
    });
}

// A subscription in the publisher's context and one in another context each get every message
// once: the broker serves the first, the rmw the second, and neither is delivered twice.
TEST_F(Broker, same_and_other_context_each_receive_once)
{
  auto other = std::make_shared<rclcpp::Context>();
  other->init(0, nullptr);
  rclcpp::NodeOptions other_options;
  other_options.context(other);
  auto node = std::make_shared<rclcpp::Node>("broker_local");
  auto remote_node = std::make_shared<rclcpp::Node>("broker_remote", other_options);
  Received local, remote;
  auto local_sub = subscribe(*node, local);
  auto remote_sub = subscribe(*remote_node, remote);
  auto publisher = node->create_publisher<Payload>("broker_payload", 20);
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);
  rclcpp::ExecutorOptions remote_executor_options;
  remote_executor_options.context = other;
  rclcpp::executors::SingleThreadedExecutor remote_executor(remote_executor_options);
  remote_executor.add_node(remote_node);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  // Both ends must match before the volatile messages are sent.
  while ((publisher->get_subscription_count() < 2 || local_sub->get_publisher_count() < 1 ||
    remote_sub->get_publisher_count() < 1) && std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(2u, publisher->get_subscription_count());
  ASSERT_EQ(1u, remote_sub->get_publisher_count());

  constexpr uint64_t count = 10;
  for (uint64_t sequence = 1; sequence <= count; ++sequence) {
    auto message = std::make_unique<Payload>();
    test_overhaul::fill(*message, sequence);
    publisher->publish(std::move(message));
    std::this_thread::sleep_for(5ms);
  }
  spin_until(executor, [&]() {return local.sequences.size() >= count;});
  spin_until(remote_executor, [&]() {return remote.sequences.size() >= count;});
  executor.spin_some(200ms);
  remote_executor.spin_some(200ms);

  std::vector<uint64_t> expected;
  for (uint64_t sequence = 1; sequence <= count; ++sequence) {
    expected.push_back(sequence);
  }
  EXPECT_EQ(expected, local.sequences);
  EXPECT_EQ(expected, remote.sequences);
  EXPECT_EQ(static_cast<int>(count), local.intact);
  EXPECT_EQ(static_cast<int>(count), remote.intact);
  EXPECT_EQ(test_overhaul::rcl_broker_enabled() ? static_cast<int>(count) : 0, local.intra_process);
  EXPECT_EQ(0, remote.intra_process);
  remote_node.reset();
  other->shutdown("test done");
}

// A deferred response arrives once per request even when the server answers twice; the second
// send_response succeeds without delivering anything.
TEST_F(Broker, deferred_and_duplicate_responses)
{
  auto node = std::make_shared<rclcpp::Node>("broker_service");
  std::vector<std::pair<rmw_request_id_t, int64_t>> pending;
  auto service = node->create_service<Defer>(
    "broker_deferred",
    [&pending](std::shared_ptr<rmw_request_id_t> header, std::shared_ptr<Defer::Request> request)
    {
      pending.emplace_back(*header, request->value);
    });
  int answered_twice = 0;
  auto timer = node->create_wall_timer(
    20ms, [&]() {
      for (auto & [header, value] : pending) {
        Defer::Response response;
        response.value = 2 * value;
        service->send_response(header, response);
        service->send_response(header, response);
        ++answered_twice;
      }
      pending.clear();
    });
  auto client = node->create_client<Defer>("broker_deferred");
  ASSERT_TRUE(client->wait_for_service(10s));
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);

  std::vector<int64_t> answers;
  constexpr int64_t calls = 5;
  for (int64_t value = 1; value <= calls; ++value) {
    auto request = std::make_shared<Defer::Request>();
    request->value = value;
    client->async_send_request(
      request, [&answers](rclcpp::Client<Defer>::SharedFuture future) {
        answers.push_back(future.get()->value);
      });
  }
  spin_until(executor, [&]() {return answers.size() >= calls;});
  executor.spin_some(300ms);
  const auto deadline = std::chrono::steady_clock::now() + 300ms;
  while (std::chrono::steady_clock::now() < deadline) {
    executor.spin_some(10ms);
  }
  EXPECT_EQ(calls, answered_twice);
  EXPECT_EQ((std::vector<int64_t>{2, 4, 6, 8, 10}), answers);
  EXPECT_EQ(0u, client->prune_pending_requests());
}
