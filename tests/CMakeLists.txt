include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

option(CORSANO_ONLINE_TESTS "Register tests that need a real bracelet" OFF)

# MARK: - Offline

add_executable(corsano_tests test_protocol.cpp test_files.cpp)
target_link_libraries(corsano_tests PRIVATE corsano_protocol Catch2::Catch2WithMain)
target_compile_definitions(corsano_tests PRIVATE CORSANO_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
catch_discover_tests(corsano_tests)

# MARK: - Online (needs CORSANO_ADDRESS or CORSANO_ADDR)

add_executable(corsano_online_tests test_online.cpp test_monitor.cpp)
target_link_libraries(corsano_online_tests PRIVATE corsano Catch2::Catch2WithMain)
if(CORSANO_ONLINE_TESTS)
  add_test(NAME corsano_online_tests COMMAND corsano_online_tests)
endif()

# MARK: - C API (pure C17, no device)

if(TARGET corsano_c)
  add_executable(corsano_c_tests test_c_api.c)
  target_link_libraries(corsano_c_tests PRIVATE corsano_c)
  set_target_properties(corsano_c_tests PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
  add_test(NAME corsano_c_api COMMAND corsano_c_tests)
endif()
