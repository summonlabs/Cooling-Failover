add_library(cf_test_support STATIC support/harness.cpp)
target_link_libraries(cf_test_support PUBLIC cooling_failover)
target_include_directories(cf_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_compile_definitions(cf_test_support PUBLIC
                           CF_TEST_CHILD_PATH="$<TARGET_FILE:cf_test_child>")
cf_apply_warnings(cf_test_support)

add_executable(cf_test_child child/cf_child.cpp)
target_link_libraries(cf_test_child PRIVATE cooling_failover)
cf_apply_warnings(cf_test_child)
add_dependencies(cf_test_support cf_test_child)

# TIMEOUT 0 disables CTest's built-in default time limit: a test that hangs is a
# defect to diagnose, not something to kill and forget.
function(cf_add_test name)
  add_executable(${name} ${name}.cpp support/test_main.cpp)
  target_link_libraries(${name} PRIVATE cf_test_support)
  cf_apply_warnings(${name})
  add_dependencies(${name} cf_test_child)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 0)
endfunction()

cf_add_test(test_core)
cf_add_test(test_eligibility)
cf_add_test(test_transition)
cf_add_test(test_persistence)
cf_add_test(test_process)
cf_add_test(test_recovery)
cf_add_test(test_determinism)
cf_add_test(test_bounds)
cf_add_test(test_adversarial)
