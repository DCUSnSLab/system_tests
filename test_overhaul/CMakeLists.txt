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
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/Payload.msg"
  "srv/Defer.srv"
)
rosidl_get_typesupport_target(cpp_typesupport_target "${PROJECT_NAME}" "rosidl_typesupport_cpp")

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_cmake_pytest REQUIRED)
  find_package(rmw_implementation_cmake REQUIRED)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()

  ament_find_gtest()

  ament_add_gtest_executable(test_core_executor test/test_core_executor.cpp)
  target_link_libraries(test_core_executor "${cpp_typesupport_target}" rclcpp::rclcpp)

  function(test_target)
    set(target_suffix "__${rmw_implementation}")
    set(rmw_implementation_env_var RMW_IMPLEMENTATION=${rmw_implementation})

    ament_add_gtest_test(test_core_executor
      TEST_NAME test_core_executor${target_suffix}
      TIMEOUT 90
      ENV ${rmw_implementation_env_var})

    foreach(test_name test_rclpy_executors test_ros2topic)
      ament_add_pytest_test(${test_name}${target_suffix} test/${test_name}.py
        TIMEOUT 120
        ENV ${rmw_implementation_env_var})
    endforeach()
  endfunction()

  call_for_each_rmw_implementation(test_target)
endif()

ament_export_dependencies(rosidl_default_runtime)
ament_package()
