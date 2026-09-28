cmake_minimum_required(VERSION 3.20)

project(CoolingFailoverConsumer VERSION 1.0.0 LANGUAGES CXX)

# This project is deliberately independent of the Cooling Failover build tree:
# it only ever sees the installed package through find_package.
find_package(CoolingFailover 1.0 REQUIRED CONFIG)

add_executable(consumer_failover main.cpp)
target_link_libraries(consumer_failover PRIVATE CoolingFailover::cooling_failover)
set_target_properties(consumer_failover PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

message(STATUS "CoolingFailover found in ${CoolingFailover_DIR}")
message(STATUS "CoolingFailover version ${CoolingFailover_VERSION}")
