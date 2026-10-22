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

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "rmw/rmw.h"

#include "test_overhaul/msg/payload.hpp"
#include "test_overhaul/srv/defer.hpp"

using test_overhaul::msg::Payload;
using test_overhaul::srv::Defer;
using namespace std::chrono_literals;

class CoreExecutor : public ::testing::Test
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
};

void spin_until(rclcpp::Executor & executor, std::function<bool()> condition)
{
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!condition() && std::chrono::steady_clock::now() < deadline) {
    executor.spin_some(10ms);
  }
}

// ROS_EXECUTOR_CORE is read when an executor is constructed.
std::unique_ptr<rclcpp::Executor> make_executor(const char * core)
{
  std::optional<std::string> saved;
  if (const char * value = std::getenv("ROS_EXECUTOR_CORE")) {
    saved = value;
  }
  if (core) {
    setenv("ROS_EXECUTOR_CORE", core, 1);
  } else {
    unsetenv("ROS_EXECUTOR_CORE");
  }
  auto executor = std::make_unique<rclcpp::executors::SingleThreadedExecutor>();
  if (saved) {
    setenv("ROS_EXECUTOR_CORE", saved->c_str(), 1);
  } else {
    unsetenv("ROS_EXECUTOR_CORE");
  }
  return executor;
}

// Everything is ready before spin_some: a due timer, three subscriptions with one or two queued
// messages, published in reverse creation order, and a service with a pending request. The
// stock order follows the executor's entity collection, so both executors see the same entities.
class ReadyWork
{
public:
  ReadyWork()
  : node_(std::make_shared<rclcpp::Node>("order")),
    peer_(std::make_shared<rclcpp::Node>("order_peer"))
  {
    timer_ = node_->create_wall_timer(
      1ms, [this]() {
        order_.push_back("timer");
        timer_->cancel();
      });
    for (const char * name : {"a", "b", "c"}) {
      subscriptions_.push_back(
        node_->create_subscription<Payload>(
          std::string("order_") + name, 5,
          [this, name](const Payload &) {order_.push_back(name);}));
      publishers_.push_back(peer_->create_publisher<Payload>(std::string("order_") + name, 5));
    }
    service_ = node_->create_service<Defer>(
      "order_service",
      [this](const std::shared_ptr<Defer::Request>, std::shared_ptr<Defer::Response>) {
        order_.push_back("service");
      });
    client_ = peer_->create_client<Defer>("order_service");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (size_t i = 0; i < publishers_.size(); ++i) {
      while ((publishers_[i]->get_subscription_count() == 0 ||
        subscriptions_[i]->get_publisher_count() == 0) &&
        std::chrono::steady_clock::now() < deadline)
      {
        std::this_thread::sleep_for(10ms);
      }
    }
    EXPECT_TRUE(client_->wait_for_service(10s));
  }

  std::vector<std::string> run(const char * core)
  {
    order_.clear();
    timer_->reset();
    for (size_t i : {2, 1, 0, 1}) {
      publishers_[i]->publish(Payload());
    }
    client_->async_send_request(std::make_shared<Defer::Request>());
    wait_until_ready();
    // The second message on "b" travels separately; give it the same time to arrive.
    std::this_thread::sleep_for(200ms);
    auto executor = make_executor(core);
    executor->add_node(node_);
    executor->spin_some();
    executor->spin_some();
    executor->remove_node(node_);
    return order_;
  }

private:
  void wait_until_ready()
  {
    rclcpp::WaitSet wait_set;
    for (auto & subscription : subscriptions_) {
      wait_set.add_subscription(subscription);
    }
    wait_set.add_service(service_);
    bool all_ready = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!all_ready && std::chrono::steady_clock::now() < deadline) {
      auto result = wait_set.wait(100ms);
      if (result.kind() != rclcpp::WaitResultKind::Ready) {
        continue;
      }
      const auto & handles = result.get_wait_set().get_rcl_wait_set();
      all_ready = handles.services[0] != nullptr;
      for (size_t i = 0; i < handles.size_of_subscriptions; ++i) {
        all_ready = all_ready && handles.subscriptions[i] != nullptr;
      }
    }
    EXPECT_TRUE(all_ready);
    for (auto & subscription : subscriptions_) {
      wait_set.remove_subscription(subscription);
    }
    wait_set.remove_service(service_);
  }

  rclcpp::Node::SharedPtr node_;
  rclcpp::Node::SharedPtr peer_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::vector<rclcpp::Subscription<Payload>::SharedPtr> subscriptions_;
  std::vector<rclcpp::Publisher<Payload>::SharedPtr> publishers_;
  rclcpp::Service<Defer>::SharedPtr service_;
  rclcpp::Client<Defer>::SharedPtr client_;
  std::vector<std::string> order_;
};

TEST_F(CoreExecutor, ready_work_runs_in_the_stock_order)
{
  ReadyWork work;
  auto stock = work.run(nullptr);
  auto core = work.run("1");
  EXPECT_EQ(stock, core);
  ASSERT_EQ(6u, stock.size());
  EXPECT_EQ("timer", stock.front());
  EXPECT_EQ("service", stock[4]);
  EXPECT_EQ("b", stock.back());
}

// A user's readiness callbacks keep firing while the executor core registers its own.
TEST_F(CoreExecutor, user_and_executor_callbacks_both_run)
{
  auto node = std::make_shared<rclcpp::Node>("fan_out");
  std::atomic<size_t> user_messages{0}, user_requests{0}, user_responses{0};
  int messages = 0;
  auto subscription = node->create_subscription<Payload>(
    "fan_out", 10, [&messages](const Payload &) {++messages;});
  subscription->set_on_new_message_callback(
    [&user_messages](size_t count) {user_messages += count;});
  auto service = node->create_service<Defer>(
    "fan_out_service",
    [](const std::shared_ptr<Defer::Request> request, std::shared_ptr<Defer::Response> response) {
      response->value = request->value;
    });
  service->set_on_new_request_callback([&user_requests](size_t count) {user_requests += count;});
  auto client = node->create_client<Defer>("fan_out_service");
  client->set_on_new_response_callback(
    [&user_responses](size_t count) {user_responses += count;});
  auto publisher = node->create_publisher<Payload>("fan_out", 10);
  ASSERT_TRUE(client->wait_for_service(10s));
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while ((publisher->get_subscription_count() == 0 || subscription->get_publisher_count() == 0) &&
    std::chrono::steady_clock::now() < deadline)
  {
    std::this_thread::sleep_for(10ms);
  }

  auto executor = make_executor("1");
  executor->add_node(node);
  int responses = 0;
  for (int i = 0; i < 3; ++i) {
    client->async_send_request(
      std::make_shared<Defer::Request>(),
      [&responses](rclcpp::Client<Defer>::SharedFuture) {++responses;});
  }
  for (int i = 0; i < 5; ++i) {
    publisher->publish(Payload());
  }
  spin_until(*executor, [&]() {return messages == 5 && responses == 3;});
  spin_until(
    *executor, [&]() {return user_messages == 5 && user_requests == 3 && user_responses == 3;});
  EXPECT_EQ(5, messages);
  EXPECT_EQ(3, responses);
  EXPECT_EQ(5u, user_messages.load());
  EXPECT_EQ(3u, user_requests.load());
  EXPECT_EQ(3u, user_responses.load());
}

// Each executed QoS event reports a change: an event is consumed once, not executed again empty.
TEST_F(CoreExecutor, matched_events_run_once_per_change)
{
  auto node = std::make_shared<rclcpp::Node>("matched_events");
  auto peer = std::make_shared<rclcpp::Node>("matched_events_peer");
  std::vector<rclcpp::MatchedInfo> events;
  rclcpp::PublisherOptions options;
  options.event_callbacks.matched_callback = [&events](rclcpp::MatchedInfo & info) {
      events.push_back(info);
    };
  auto publisher = node->create_publisher<Payload>("matched_events", 10, options);
  auto executor = make_executor("1");
  executor->add_node(node);
  auto current = [&events]() {return events.empty() ? 0 : events.back().current_count;};
  auto first = peer->create_subscription<Payload>("matched_events", 10, [](const Payload &) {});
  spin_until(*executor, [&]() {return current() == 1;});
  auto second = peer->create_subscription<Payload>("matched_events", 10, [](const Payload &) {});
  spin_until(*executor, [&]() {return current() == 2;});
  first.reset();
  spin_until(*executor, [&]() {return current() == 1;});
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (std::chrono::steady_clock::now() < deadline) {
    executor->spin_some(10ms);
  }

  ASSERT_FALSE(events.empty());
  EXPECT_EQ(1u, events.back().current_count);
  int32_t current_changes = 0;
  for (const auto & event : events) {
    EXPECT_TRUE(event.total_count_change != 0 || event.current_count_change != 0);
    current_changes += event.current_count_change;
  }
  EXPECT_EQ(1, current_changes);
}

// DDS keeps a missed deadline ready after it is taken. The core consumes the event once instead
// of executing it again with no change, as system_tests test_deadline expects.
TEST_F(CoreExecutor, deadline_events_report_a_change)
{
  if (std::strcmp(rmw_get_implementation_identifier(), "rmw_zenoh_cpp") == 0) {
    GTEST_SKIP() << "rmw_zenoh_cpp has no deadline events";
  }
  auto node = std::make_shared<rclcpp::Node>("deadline_events");
  std::vector<rclcpp::QOSDeadlineOfferedInfo> events;
  rclcpp::PublisherOptions options;
  options.event_callbacks.deadline_callback = [&events](rclcpp::QOSDeadlineOfferedInfo & info) {
      events.push_back(info);
    };
  auto qos = rclcpp::QoS(10).deadline(rclcpp::Duration(50ms));
  auto publisher = node->create_publisher<Payload>("deadline_events", qos, options);
  auto executor = make_executor("1");
  executor->add_node(node);
  publisher->publish(Payload());
  const auto deadline = std::chrono::steady_clock::now() + 600ms;
  while (std::chrono::steady_clock::now() < deadline) {
    executor->spin_some(10ms);
  }

  ASSERT_FALSE(events.empty());
  int32_t changes = 0;
  for (const auto & event : events) {
    EXPECT_GE(event.total_count_change, 1);
    changes += event.total_count_change;
  }
  EXPECT_EQ(events.back().total_count, changes);
}
