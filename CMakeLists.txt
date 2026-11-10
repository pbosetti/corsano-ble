cmake_minimum_required(VERSION 3.24)
project(corsano LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # static SimpleBLE into a shared lib

option(CORSANO_BUILD_TESTS "Build tests" ON)
option(CORSANO_BUILD_CLI "Build CLI" ON)
option(CORSANO_BUILD_C "Build the C API shared library" ON)

# On Linux SimpleBLE needs libdbus-1-dev.
include(FetchContent)
FetchContent_Declare(simpleble
  GIT_REPOSITORY https://github.com/simpleble/simpleble.git
  GIT_TAG v1.4.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR simpleble)
FetchContent_MakeAvailable(simpleble)

# Pure codec, no BLE dependency (offline tests link this)
add_library(corsano_protocol STATIC src/protocol.cpp src/files.cpp)
target_include_directories(corsano_protocol PUBLIC include)
target_compile_features(corsano_protocol PUBLIC cxx_std_20)
set_target_properties(corsano_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(corsano_protocol PRIVATE -Wall -Wextra)

add_library(corsano STATIC src/bracelet.cpp src/monitor.cpp)
add_library(corsano::corsano ALIAS corsano)
target_include_directories(corsano PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(corsano PUBLIC corsano_protocol PRIVATE simpleble::simpleble Threads::Threads)
set_target_properties(corsano PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(corsano PRIVATE -Wall -Wextra)

if(CORSANO_BUILD_C)
  add_library(corsano_c SHARED src/corsano_c.cpp)
  target_include_directories(corsano_c PUBLIC include)
  target_link_libraries(corsano_c PRIVATE corsano)
  target_compile_options(corsano_c PRIVATE -Wall -Wextra)
endif()

if(CORSANO_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
  enable_testing()
  add_subdirectory(tests)
endif()
if(CORSANO_BUILD_CLI AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cli/CMakeLists.txt)
  add_subdirectory(cli)
endif()
