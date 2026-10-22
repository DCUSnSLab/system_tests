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
import signal
import subprocess
import threading
import time

from ament_index_python.packages import get_package_prefix
from composition_interfaces.srv import LoadNode
import rclpy
from rclpy.executors import SingleThreadedExecutor


def read(path):
    try:
        with open(path) as stream:
            return {key: int(value) for key, value in
                    (line.split() for line in stream if line.strip())}
    except FileNotFoundError:
        return {}


def test_cpp_and_python_exchange_messages_and_deferred_responses(tmp_path):
    """C++ and Python nodes of one mixed container talk through one rcl context."""
    container = os.path.join(
        get_package_prefix('rclpy_components'), 'lib', 'rclpy_components',
        'component_container_mixed')
    env = dict(os.environ, TEST_OVERHAUL_REPORT=str(tmp_path))
    log = open(tmp_path / 'container.log', 'w+')
    process = subprocess.Popen(
        [container, '--ros-args', '-r', '__node:=overhaul_mixed'],
        env=env, stdout=log, stderr=subprocess.STDOUT)
    context = rclpy.context.Context()
    rclpy.init(context=context)
    node = rclpy.create_node('overhaul_mixed_client', context=context)
    executor = SingleThreadedExecutor(context=context)
    executor.add_node(node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()
    try:
        load = node.create_client(LoadNode, '/overhaul_mixed/_container/load_node')
        assert load.wait_for_service(timeout_sec=20.0)
        for package, plugin in (('test_overhaul', 'test_overhaul::BrokerPeer'),
                                ('test_overhaul', 'broker_fixtures:PyPeer')):
            response = load.call(LoadNode.Request(package_name=package, plugin_name=plugin))
            assert response.success, response.error_message

        def done():
            cpp, py = read(tmp_path / 'cpp'), read(tmp_path / 'py')
            return cpp.get('received', 0) >= 20 and py.get('received', 0) >= 20 and \
                py.get('answers', 0) == 5 and cpp.get('answered_twice', 0) == 5
        deadline = time.monotonic() + 30.0
        while not done() and time.monotonic() < deadline:
            time.sleep(0.1)
        time.sleep(0.5)
        cpp, py = read(tmp_path / 'cpp'), read(tmp_path / 'py')
        assert cpp.get('received', 0) >= 20, cpp
        assert cpp['intact'] == cpp['received'], cpp
        assert cpp['expected_route'] == cpp['received'], cpp
        assert cpp['answered_twice'] == 5, cpp
        assert py.get('received', 0) >= 20, py
        assert py['intact'] == py['received'], py
        assert py['answers'] == 5 and py['correct'] == 5, py
        assert process.poll() is None
        process.send_signal(signal.SIGINT)
        assert process.wait(timeout=10) == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        node.destroy_node()
        executor.shutdown()
        thread.join()
        rclpy.shutdown(context=context)
        log.seek(0)
        output = log.read()
        log.close()
        print(output)
    assert 'Traceback' not in output
