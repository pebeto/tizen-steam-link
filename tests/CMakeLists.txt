add_executable(test-core test_core.cpp)
target_link_libraries(test-core PRIVATE tsl-core)
add_test(NAME core COMMAND test-core)
