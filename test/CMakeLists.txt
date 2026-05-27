cmake_minimum_required(VERSION 3.22)

include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG
    6ee0826dcae55ed1e06b2c5701981221e979e1e6 # v3.15.0
  GIT_SHALLOW TRUE
  SYSTEM
)
FetchContent_MakeAvailable(Catch2)

add_executable(unittests test_basic.cpp)

target_include_directories(unittests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/test)
target_include_directories(unittests PRIVATE SYSTEM ${duckdb_SOURCE_DIR}/src/include)

target_link_libraries(
  unittests
  PRIVATE
    # ${EXTENSION_NAME} pulls duckdb_static transitively (build_static_extension
    # links it without keyword → propagates). Listing it explicitly here would
    # double-list the .a and trip ld's "ignoring duplicate libraries" warning.
    ${EXTENSION_NAME}
    duckdb_generated_extension_loader
    parquet_extension
    core_functions_extension
    Catch2::Catch2WithMain
)

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(unittests)
