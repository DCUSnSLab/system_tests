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

import re
import signal
import subprocess
import threading
import time

import pytest
import rclpy
from test_overhaul.msg import Payload

RATE = 50.0


@pytest.fixture()
def publisher():
    context = rclpy.context.Context()
    rclpy.init(context=context)
    node = rclpy.create_node('topic_rate_source', context=context)
    pub = node.create_publisher(Payload, 'topic_rate', 10)
    running = True

    def run():
        sequence = 0
        next_time = time.monotonic()
        while running:
            sequence += 1
            pub.publish(Payload(sequence=sequence))
            next_time += 1.0 / RATE
            time.sleep(max(0.0, next_time - time.monotonic()))
    thread = threading.Thread(target=run)
    thread.start()
    yield
    running = False
    thread.join()
    node.destroy_node()
    rclpy.shutdown(context=context)


# ros2cli exits with SIGINT's number after a KeyboardInterrupt.
def run_verb(verb, seconds):
    process = subprocess.Popen(
        ['ros2', 'topic', verb, '/topic_rate', '--window', '50'],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(seconds)
    process.send_signal(signal.SIGINT)
    output, _ = process.communicate(timeout=10)
    return process.returncode, output


def test_hz_reports_the_publish_rate(publisher):
    returncode, output = run_verb('hz', 6.0)
    assert returncode == signal.SIGINT, output
    rates = [float(rate) for rate in re.findall(r'average rate: ([0-9.]+)', output)]
    assert rates, output
    assert abs(rates[-1] - RATE) < 0.2 * RATE, output


def test_bw_reports_the_bandwidth(publisher):
    returncode, output = run_verb('bw', 6.0)
    assert returncode == signal.SIGINT, output
    units = {'B': 1, 'KB': 1000, 'MB': 1000 ** 2}
    rates = re.findall(r'([0-9.]+) ([KM]?B)/s from', output)
    means = re.findall(r'Message size mean: ([0-9.]+) ([KM]?B)', output)
    assert rates and means, output
    size = 4 + 8 + 65536
    rate = float(rates[-1][0]) * units[rates[-1][1]]
    mean = float(means[-1][0]) * units[means[-1][1]]
    # bw prints two decimals.
    assert abs(mean - size) <= 0.01 * units[means[-1][1]], output
    assert abs(rate - size * RATE) < 0.2 * size * RATE, output
