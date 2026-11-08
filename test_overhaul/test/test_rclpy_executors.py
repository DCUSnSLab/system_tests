# Copyright 2026 nev
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import threading
import time

import pytest
import rclpy
from rclpy.duration import Duration
from rclpy.executors import MultiThreadedExecutor
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from rclpy.parameter import Parameter
from rosgraph_msgs.msg import Clock
from std_msgs.msg import String


@pytest.fixture()
def context():
    context = rclpy.context.Context()
    rclpy.init(context=context)
    yield context
    rclpy.shutdown(context=context)


def wait_for(predicate, timeout=10.0):
    deadline = time.monotonic() + timeout
    while not predicate() and time.monotonic() < deadline:
        time.sleep(0.01)
    return predicate()


class SimClock:
    """Publish /clock from another context, 10 ms of ROS time every 10 ms."""

    def __init__(self):
        self.context = rclpy.context.Context()
        rclpy.init(context=self.context)
        self.node = rclpy.create_node('sim_clock', context=self.context)
        self.publisher = self.node.create_publisher(Clock, '/clock', 10)
        self.running = True
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def run(self):
        nanoseconds = 1_000_000_000
        while self.running:
            nanoseconds += 10_000_000
            msg = Clock()
            msg.clock.sec, msg.clock.nanosec = divmod(nanoseconds, 1_000_000_000)
            self.publisher.publish(msg)
            time.sleep(0.01)

    def stop(self):
        self.running = False
        self.thread.join()
        self.node.destroy_node()
        rclpy.shutdown(context=self.context)


@pytest.mark.parametrize('executor_type,use_clock_thread', [
    (MultiThreadedExecutor, False), (SingleThreadedExecutor, True)])
def test_ros_time_advances_while_a_callback_waits_for_it(
        context, executor_type, use_clock_thread):
    node = Node(
        'sim_time_user', context=context, use_clock_thread=use_clock_thread,
        parameter_overrides=[Parameter('use_sim_time', value=True)])
    clock = SimClock()
    advanced = []

    def wait_for_ros_time(msg):
        start = node.get_clock().now()
        deadline = time.monotonic() + 5.0
        while node.get_clock().now() - start < Duration(seconds=0.5) and \
                time.monotonic() < deadline:
            time.sleep(0.01)
        advanced.append(node.get_clock().now() - start)

    node.create_subscription(String, 'sim_time_trigger', wait_for_ros_time, 10)
    publisher = clock.node.create_publisher(String, 'sim_time_trigger', 10)
    executor = executor_type(context=context)
    executor.add_node(node)
    thread = threading.Thread(target=executor.spin)
    thread.start()
    try:
        assert wait_for(lambda: node.get_clock().now().nanoseconds > 0)
        assert wait_for(lambda: publisher.get_subscription_count() > 0 and
                        node.count_publishers('sim_time_trigger') > 0)
        publisher.publish(String())
        assert wait_for(lambda: advanced, timeout=15.0)
        assert advanced[0] >= Duration(seconds=0.5)
    finally:
        executor.shutdown()
        thread.join()
        node.destroy_node()
        clock.stop()
