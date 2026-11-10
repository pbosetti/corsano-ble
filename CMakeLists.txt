cmake_minimum_required(VERSION 3.24)
project(corsano VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # static SimpleBLE into a shared lib

# Extras default to ON only when built on its own, so a FetchContent consumer
# gets just the libraries (and no Catch2/cxxopts downloads)
option(CORSANO_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(CORSANO_BUILD_CLI "Build CLI" ${PROJECT_IS_TOP_LEVEL})
option(CORSANO_BUILD_C "Build the C API shared library" ${PROJECT_IS_TOP_LEVEL})
option(CORSANO_BUILD_DOCS "Build Doxygen HTML documentation (target: doc)" OFF)

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
add_library(corsano::protocol ALIAS corsano_protocol)
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
  add_library(corsano::c ALIAS corsano_c)
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

if(CORSANO_BUILD_DOCS)
  find_package(Doxygen REQUIRED OPTIONAL_COMPONENTS dot)
  set(DOXYGEN_PROJECT_NAME corsano-ble)
  set(DOXYGEN_PROJECT_BRIEF "C++20 BLE library and C API for Corsano bracelets")
  set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/doc)
  set(DOXYGEN_GENERATE_HTML YES)
  set(DOXYGEN_GENERATE_LATEX NO)
  set(DOXYGEN_EXTRACT_ALL NO)
  set(DOXYGEN_WARN_AS_ERROR NO)
  set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE ${CMAKE_CURRENT_SOURCE_DIR}/README.md)
  set(DOXYGEN_FILE_PATTERNS *.hpp *.h *.md)
  set(DOXYGEN_EXCLUDE_SYMBOLS "*Impl*")
  set(DOXYGEN_STRIP_FROM_PATH ${CMAKE_CURRENT_SOURCE_DIR}/include)
  set(DOXYGEN_MARKDOWN_SUPPORT YES)
  set(DOXYGEN_ENABLE_PREPROCESSING YES)
  set(DOXYGEN_MACRO_EXPANSION YES)
  set(DOXYGEN_PREDEFINED __cplusplus=202002L)
  if(DOXYGEN_DOT_FOUND)
    set(DOXYGEN_HAVE_DOT YES)
  endif()
  doxygen_add_docs(doc include/corsano README.md docs cli/README.md
    python/README.md
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Doxygen HTML in build/doc/html")
endif()
