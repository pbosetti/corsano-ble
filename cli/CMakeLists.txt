include(FetchContent)
FetchContent_Declare(
  cxxopts
  GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
  GIT_TAG v3.2.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(cxxopts)

add_executable(corsano-cli main.cpp)
target_compile_features(corsano-cli PRIVATE cxx_std_20)
target_link_libraries(corsano-cli PRIVATE corsano cxxopts::cxxopts)
