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

import os

import numpy
from rclpy.node import Node
from test_overhaul.msg import Payload
from test_overhaul.srv import Defer

CALLS = 5


def pattern(sequence):
    return ((numpy.arange(65536, dtype=numpy.uint64) + sequence) & 0xff).astype(numpy.uint8)


class PyPeer(Node):
    """Python side of the mixed container test, loaded next to test_overhaul::BrokerPeer."""

    def __init__(self, **kwargs):
        super().__init__('py_peer', **kwargs)
        self.report = os.path.join(os.environ['TEST_OVERHAUL_REPORT'], 'py')
        self.received = 0
        self.intact = 0
        self.answers = []
        self.calls = 0
        self.sequence = 0
        self.publisher = self.create_publisher(Payload, 'py_to_cpp', 10)
        self.create_subscription(Payload, 'cpp_to_py', self.on_payload, 10)
        self.client = self.create_client(Defer, 'deferred')
        self.create_timer(0.05, self.on_timer)

    def on_payload(self, msg):
        self.received += 1
        self.intact += bool(numpy.array_equal(msg.data, pattern(msg.sequence)))

    def on_timer(self):
        self.sequence += 1
        msg = Payload(sequence=self.sequence)
        msg.data = pattern(self.sequence)
        self.publisher.publish(msg)
        if self.calls < CALLS and self.calls == len(self.answers) and \
                self.client.service_is_ready():
            self.calls += 1
            value = self.calls
            future = self.client.call_async(Defer.Request(value=value))
            future.add_done_callback(
                lambda done: self.answers.append(done.result().value == 2 * value))
        with open(self.report + '.partial', 'w') as stream:
            stream.write(f'received {self.received}\nintact {self.intact}\n'
                         f'answers {len(self.answers)}\ncorrect {sum(self.answers)}\n')
        os.replace(self.report + '.partial', self.report)
