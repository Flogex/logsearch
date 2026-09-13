cmake_minimum_required(VERSION 3.28...4.4)
project(LogsearchExtension CXX)

include(FetchContent)

set(EXTENSION_NAME logsearch_extension)
set(LOADABLE_EXTENSION_NAME logsearch_loadable_extension)

option(ENABLE_LINTING "Run clang-tidy and cppcheck on extension targets during build" OFF)
# We provide our own option to control unity builds instead of relying on CMAKE_UNITY_BUILD.
# CMAKE_UNITY_BUILD initializes UNITY_BUILD on every target, but we want a separate knob for Logsearch.
option(LOGSEARCH_UNITY_BUILD "Compile subcomponents as one translation unit each" ON)
option(LOGSEARCH_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(LOGSEARCH_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)

# A unity build's compilation database names generated unity sources rather than our own files, so
# cppcheck's file filter skips them and clangd and iwyu_tool.py cannot attribute anything to a
# single source.
if(ENABLE_LINTING AND LOGSEARCH_UNITY_BUILD)
  message(
    FATAL_ERROR
    "ENABLE_LINTING requires LOGSEARCH_UNITY_BUILD=OFF. Pass -DLOGSEARCH_UNITY_BUILD=OFF or use "
    "the release-lint preset."
  )
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/linters.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/sanitizers.cmake)

# We set DUCKDB_EXTENSION_CONFIGS before adding DuckDB as subdirectory.
# DuckDB reads extension_config.cmake and calls add_subdirectory again to load this directory.
# The else branch covers re-entrance. On re-entrance, DuckDB's CMake functions are available.
if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(COMPILER_LAUNCHER NAMES ccache sccache)
    if(NOT COMPILER_LAUNCHER)
      message(
        WARNING
        "(s)ccache not found. Using a compiler cache is recommended. Configure one "
        "by exporting the CMAKE_CXX_COMPILER_LAUNCHER environment variable. Pass "
        "-DCMAKE_CXX_COMPILER_LAUNCHER= to silence this warning."
      )
    endif()
  endif()

  # Enable sanitizers in DuckDB if and only if they are enabled for the extension targets as well.
  # UBSAN is only enabled in debug mode or if ENABLE_UBSAN and FORCE_ASSERT are set to true.
  # This means that in our setup, the DuckDB code will never be instrumented with UBSAN in release mode.
  set(ENABLE_SANITIZER ${LOGSEARCH_ENABLE_ASAN})
  set(RELEASE_SANITIZER ${LOGSEARCH_ENABLE_ASAN})
  set(ENABLE_UBSAN ${LOGSEARCH_ENABLE_UBSAN})

  set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
  set(FETCHCONTENT_QUIET OFF)

  # Tell DuckDB to bake logsearch into duckdb_static and register its SQLLogicTests so
  # the duckdb unittest binary can resolve `require logsearch`. DuckDB consumes this
  # during its own CMake processing and calls `add_subdirectory` back into this file. The
  # nested project() call there has PROJECT_IS_TOP_LEVEL=FALSE, so we fall through to
  # the else-branch below.
  set(
    DUCKDB_EXTENSION_CONFIGS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/extension_config.cmake"
    CACHE STRING
    ""
    FORCE
  )
  FetchContent_Declare(
    duckdb
    GIT_REPOSITORY https://github.com/duckdb/duckdb.git
    GIT_TAG
      e500d77864fe565f90e68f06d729c25b11e775c5 # main branch as of 2026-08-09
    # `GIT_SHALLOW TRUE` only works with branch names and tags.
    GIT_SHALLOW FALSE
    SYSTEM
  )
  set(ENABLE_UNITTEST_CPP_TESTS OFF) # Don't build DuckDB C++ tests, which we don't run anyway.
  FetchContent_MakeAvailable(duckdb)

  add_subdirectory(test)
else()
  # Re-entry from DuckDB's CMakeLists:
  # DuckDB's extension_build_tools.cmake called add_subdirectory(${CMAKE_CURRENT_LIST_DIR}) for us.

  # DuckDB uses old-style include_directories(). Hence, we have a bunch of directories in INCLUDE_DIRECTORIES now
  # that are added with -I, not -isystem, to the compiler. This means we will get compiler warnings from DuckDB's headers.
  # To avoid that, we re-add DuckDB's include directories as SYSTEM.
  get_directory_property(_inherited_inc INCLUDE_DIRECTORIES)
  if(_inherited_inc)
    set_directory_properties(PROPERTIES INCLUDE_DIRECTORIES "")
    foreach(d IN LISTS _inherited_inc)
      include_directories(SYSTEM ${d})
    endforeach()
  endif()

  add_subdirectory(src)
endif()
