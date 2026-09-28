cmake_minimum_required(VERSION 3.20)

project(
  CoolingFailover
  VERSION 1.0.0
  DESCRIPTION "Cooling failover orchestration for the Data Center Control Plane"
  HOMEPAGE_URL "https://github.com/summonlabs/Cooling-Failover"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(COOLING_FAILOVER_BUILD_TESTS "Build the Cooling Failover test suite" ON)
option(COOLING_FAILOVER_BUILD_TOOLS "Build the cfctl administration CLI" ON)
option(COOLING_FAILOVER_BUILD_EXAMPLES "Build the runnable examples" ON)
option(COOLING_FAILOVER_BUILD_BENCHMARK "Build the completed-operation benchmark" ON)
option(COOLING_FAILOVER_BUILD_SHARED "Build cooling_failover as a shared library" OFF)
option(COOLING_FAILOVER_ENABLE_ASAN "Build with AddressSanitizer where supported" OFF)
option(COOLING_FAILOVER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(COOLING_FAILOVER_BUILD_SHARED)
  set(COOLING_FAILOVER_LIBRARY_TYPE SHARED)
else()
  set(COOLING_FAILOVER_LIBRARY_TYPE STATIC)
endif()

# ---------------------------------------------------------------------------
# Strict warnings, applied directly to every first-party target.
# ---------------------------------------------------------------------------

function(cf_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus
                                            /Zc:preprocessor /EHsc)
    if(COOLING_FAILOVER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX
                                                 WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                                            -Wsign-conversion)
    if(COOLING_FAILOVER_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(COOLING_FAILOVER_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /fsanitize=address)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(COOLING_FAILOVER_SOURCES
    src/canonical.cpp
    src/eligibility.cpp
    src/ids.cpp
    src/model.cpp
    src/orchestrator.cpp
    src/plan.cpp
    src/state.cpp
    src/state_apply.cpp
    src/state_codec.cpp
    src/status.cpp
    src/store.cpp
    src/store_durable.cpp
    src/store_internal.cpp
    src/synthetic.cpp
    src/units.cpp
    src/version.cpp)

set(COOLING_FAILOVER_PUBLIC_HEADERS
    include/cooling_failover/adapters.hpp
    include/cooling_failover/canonical.hpp
    include/cooling_failover/eligibility.hpp
    include/cooling_failover/export.hpp
    include/cooling_failover/ids.hpp
    include/cooling_failover/model.hpp
    include/cooling_failover/orchestrator.hpp
    include/cooling_failover/plan.hpp
    include/cooling_failover/state.hpp
    include/cooling_failover/status.hpp
    include/cooling_failover/store.hpp
    include/cooling_failover/synthetic.hpp
    include/cooling_failover/units.hpp
    include/cooling_failover/version.hpp)

add_library(cooling_failover ${COOLING_FAILOVER_LIBRARY_TYPE} ${COOLING_FAILOVER_SOURCES}
                             ${COOLING_FAILOVER_PUBLIC_HEADERS})
add_library(CoolingFailover::cooling_failover ALIAS cooling_failover)

target_include_directories(
  cooling_failover PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
                          "$<INSTALL_INTERFACE:include>"
                   PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

cf_apply_warnings(cooling_failover)
target_compile_definitions(cooling_failover PRIVATE COOLING_FAILOVER_BUILDING_LIBRARY)
if(COOLING_FAILOVER_BUILD_SHARED)
  target_compile_definitions(cooling_failover PUBLIC COOLING_FAILOVER_SHARED)
endif()

set_target_properties(
  cooling_failover
  PROPERTIES VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             OUTPUT_NAME cooling_failover
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON)

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(
  TARGETS cooling_failover
  EXPORT CoolingFailoverTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
  EXPORT CoolingFailoverTargets
  FILE CoolingFailoverTargets.cmake
  NAMESPACE CoolingFailover::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoolingFailover)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoolingFailoverConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/CoolingFailoverConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoolingFailover)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/CoolingFailoverConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CoolingFailoverConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/CoolingFailoverConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoolingFailover)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/CoolingFailover)

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------

if(COOLING_FAILOVER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(COOLING_FAILOVER_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(COOLING_FAILOVER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(COOLING_FAILOVER_BUILD_BENCHMARK)
  add_subdirectory(bench)
endif()
