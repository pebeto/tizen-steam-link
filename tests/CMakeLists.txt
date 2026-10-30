add_executable(test-core test_core.cpp)
target_link_libraries(test-core PRIVATE tsl-core)
add_test(NAME core COMMAND test-core)

add_executable(test-counter test_counter.cpp)
target_include_directories(test-counter PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test-counter PRIVATE Threads::Threads)
add_test(NAME counter COMMAND test-counter)
