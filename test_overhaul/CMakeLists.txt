cmake_minimum_required(VERSION 3.5)

project(test_overhaul)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()
if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_components REQUIRED)
find_package(rosidl_default_generators REQUIRED)

# The mixed container loads the C++ peer through the ament index, so the interfaces and the
# component are installed.
rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/Payload.msg"
  "srv/Defer.srv"
)
rosidl_get_typesupport_target(cpp_typesupport_target "${PROJECT_NAME}" "rosidl_typesupport_cpp")

add_library(broker_peer SHARED src/broker_peer.cpp)
target_include_directories(broker_peer PRIVATE include)
target_link_libraries(broker_peer "${cpp_typesupport_target}" rclcpp::rclcpp
  rclcpp_components::component)
rclcpp_components_register_nodes(broker_peer "test_overhaul::BrokerPeer")
install(TARGETS broker_peer
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_cmake_pytest REQUIRED)
  find_package(rmw_implementation_cmake REQUIRED)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()

  ament_find_gtest()

  foreach(test_name test_broker test_core_executor test_loan_lifetime)
    ament_add_gtest_executable(${test_name} test/${test_name}.cpp)
    target_include_directories(${test_name} PRIVATE include)
    target_link_libraries(${test_name} "${cpp_typesupport_target}" rclcpp::rclcpp)
  endforeach()

  function(test_target)
    set(target_suffix "__${rmw_implementation}")
    set(rmw_implementation_env_var RMW_IMPLEMENTATION=${rmw_implementation})

    ament_add_gtest_test(test_broker
      TEST_NAME test_broker${target_suffix}
      TIMEOUT 60
      ENV ${rmw_implementation_env_var})

    ament_add_gtest_test(test_core_executor
      TEST_NAME test_core_executor${target_suffix}
      TIMEOUT 90
      ENV ${rmw_implementation_env_var})

    # Subscription loans need Zenoh shared memory.
    if(rmw_implementation STREQUAL "rmw_zenoh_cpp")
      ament_add_gtest_test(test_loan_lifetime
        TEST_NAME test_loan_lifetime${target_suffix}
        TIMEOUT 60
        ENV ${rmw_implementation_env_var} ROS_DISABLE_LOANED_MESSAGES=0
          ZENOH_CONFIG_OVERRIDE=transport/shared_memory/enabled=true)
    endif()

    foreach(test_name test_mixed_broker test_rclpy_executors test_ros2topic)
      ament_add_pytest_test(${test_name}${target_suffix} test/${test_name}.py
        TIMEOUT 120
        ENV ${rmw_implementation_env_var}
        APPEND_ENV PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/test)
    endforeach()
  endfunction()

  call_for_each_rmw_implementation(test_target)
endif()

ament_export_dependencies(rosidl_default_runtime)
ament_package()
