cmake_minimum_required(VERSION 3.16)
# (SIMDJSON_MSGPACK_COMPETITION needs CMake 3.18.)
project(simdjson_msgpack VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SIMDJSON_MSGPACK_TESTS "Build the tests" ON)
option(SIMDJSON_MSGPACK_BENCHMARKS "Build the benchmarks" ON)
option(SIMDJSON_MSGPACK_EXAMPLES "Build the examples" ON)
# The benchmark of other libraries (nlohmann/json, yyjson, RapidJSON, mpack),
# which CMake downloads.
option(SIMDJSON_MSGPACK_COMPETITION "Benchmark other libraries too" OFF)

include(FetchContent)
FetchContent_Declare(simdjson
  URL https://github.com/simdjson/simdjson/archive/refs/tags/v5.0.7.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(simdjson)

# zmij (third_party/zmij) writes floating-point numbers.
add_library(zmij OBJECT third_party/zmij/zmij.cc)

set(SIMDJSON_MSGPACK_SOURCES src/to_msgpack.cpp src/to_json.cpp src/c_api.cpp
    $<TARGET_OBJECTS:zmij>)

add_library(simdjson_msgpack STATIC ${SIMDJSON_MSGPACK_SOURCES})
target_include_directories(simdjson_msgpack PUBLIC include PRIVATE third_party/zmij)
target_link_libraries(simdjson_msgpack PUBLIC simdjson::simdjson)

# A shared library with the C entry points, for bindings and the Python tests (ctypes).
add_library(simdjson_msgpack_c SHARED ${SIMDJSON_MSGPACK_SOURCES})
target_include_directories(simdjson_msgpack_c PUBLIC include PRIVATE third_party/zmij)
target_link_libraries(simdjson_msgpack_c PRIVATE simdjson::simdjson)

if(SIMDJSON_MSGPACK_TESTS)
  enable_testing()
  add_executable(basic_tests tests/basic_tests.cpp)
  target_link_libraries(basic_tests PRIVATE simdjson_msgpack)
  add_test(NAME basic_tests COMMAND basic_tests)
endif()

if(SIMDJSON_MSGPACK_BENCHMARKS)
  add_executable(bench benchmarks/bench.cpp)
  target_link_libraries(bench PRIVATE simdjson_msgpack)
  if(SIMDJSON_MSGPACK_COMPETITION)
    # Header-only (or without a CMakeLists.txt we want): SOURCE_SUBDIR names a
    # directory that does not exist, so that FetchContent only downloads them.
    FetchContent_Declare(nlohmann_json
      URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR none)
    FetchContent_Declare(yyjson
      URL https://github.com/ibireme/yyjson/archive/refs/tags/0.12.0.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_Declare(rapidjson
      URL https://github.com/Tencent/rapidjson/archive/24b5e7a8b27f42fa16b96fc70aade9106cf7102f.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR none)
    FetchContent_Declare(mpack
      URL https://github.com/ludocode/mpack/releases/download/v1.1.1/mpack-amalgamation-1.1.1.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR none)
    FetchContent_MakeAvailable(yyjson nlohmann_json rapidjson mpack)
    add_library(mpack STATIC ${mpack_SOURCE_DIR}/src/mpack/mpack.c)
    target_include_directories(mpack PUBLIC ${mpack_SOURCE_DIR}/src/mpack)
    target_compile_definitions(mpack PUBLIC MPACK_EXTENSIONS=1)
    set_target_properties(mpack PROPERTIES LINKER_LANGUAGE C)
    enable_language(C)
    add_executable(bench_competition benchmarks/bench_competition.cpp)
    target_include_directories(bench_competition PRIVATE
      ${nlohmann_json_SOURCE_DIR}/single_include ${rapidjson_SOURCE_DIR}/include)
    target_link_libraries(bench_competition PRIVATE simdjson_msgpack yyjson mpack)
  endif()
endif()

if(SIMDJSON_MSGPACK_EXAMPLES)
  add_executable(example examples/example.cpp)
  target_link_libraries(example PRIVATE simdjson_msgpack)
  if(SIMDJSON_MSGPACK_TESTS)
    add_test(NAME example COMMAND example)
  endif()
endif()
