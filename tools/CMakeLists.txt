add_executable(cfctl cfctl.cpp)
target_link_libraries(cfctl PRIVATE cooling_failover)
cf_apply_warnings(cfctl)
install(TARGETS cfctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
