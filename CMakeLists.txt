cmake_minimum_required(VERSION 3.28...4.4)
project(LogsearchExtension CXX)

set(EXTENSION_NAME logsearch_extension)
set(LOADABLE_EXTENSION_NAME logsearch_loadable_extension)

option(LOGSEARCH_ENABLE_LINTING "Run clang-tidy and cppcheck on extension targets during build" OFF)
# We provide our own option to control unity builds instead of relying on CMAKE_UNITY_BUILD.
# CMAKE_UNITY_BUILD initializes UNITY_BUILD on every target, but we want a separate knob for Logsearch.
option(LOGSEARCH_UNITY_BUILD "Compile subcomponents as one translation unit each" ON)
option(LOGSEARCH_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(LOGSEARCH_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)

# A unity build's compilation database names generated unity sources rather than our own files, so
# cppcheck's file filter skips them and clangd and iwyu_tool.py cannot attribute anything to a
# single source.
if(LOGSEARCH_ENABLE_LINTING AND LOGSEARCH_UNITY_BUILD)
  message(
    FATAL_ERROR
    "LOGSEARCH_ENABLE_LINTING requires LOGSEARCH_UNITY_BUILD=OFF. Pass -DLOGSEARCH_UNITY_BUILD=OFF or use "
    "the release-lint preset."
  )
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/components.cmake)
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

  # We use CPM (CMake Package Manager) which is a wrapper around FetchContent but allows us to
  # share a dependency's source directory between build types. The binary directory of
  # dependencies will still be separated per build type.
  # Don't try to get system packages via find_package.
  set(CPM_USE_LOCAL_PACKAGES OFF)
  set(CPM_LOCAL_PACKAGES_ONLY OFF)
  set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
  # We keep the cache within this repo's build directory by default to not litter a developer's
  # system. The cache directory can be changed by setting CPM_SOURCE_CACHE, e.g., to share the
  # cache across git worktrees.
  if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_LIST_DIR}/build/deps" CACHE PATH "")
  endif()
  # Fallback for packages that do not pass a CUSTOM_CACHE_KEY.
  set(CPM_USE_NAMED_CACHE_DIRECTORIES ON CACHE BOOL "")
  set(FETCHCONTENT_QUIET OFF)

  # File downloaded from https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.43.1/CPM.cmake
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/CPM.cmake)

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
  # Using CPM defaults policies CMP0077/0126/0135/0150 to NEW for every subproject, which is newer
  # than 3.14...3.29 that DuckDB requests. But that doesn't break anything (yet).
  # duckdb_SOURCE_DIR is needed by test/CMakeLists.txt.
  block(SCOPE_FOR VARIABLES PROPAGATE duckdb_SOURCE_DIR)
    set(commit e500d77864fe565f90e68f06d729c25b11e775c5) # main as of 2026-08-09
    string(SUBSTRING "${commit}" 0 10 commit_short)
    CPMAddPackage(
      NAME duckdb
      GIT_REPOSITORY https://github.com/duckdb/duckdb.git
      GIT_TAG ${commit}
      # `GIT_SHALLOW TRUE` only works with branch names and tags.
      GIT_SHALLOW FALSE
      CUSTOM_CACHE_KEY "${commit_short}/duckdb"
      SYSTEM YES
      EXCLUDE_FROM_ALL NO
      # Enable sanitizers in DuckDB if and only if they are enabled for the extension targets as well.
      # DuckDB only honors ENABLE_UBSAN in debug mode or when FORCE_ASSERT is set.
      # ASAN can be enabled for release build types as well with RELEASE_SANITIZER.
      OPTIONS
        "ENABLE_UNITTEST_CPP_TESTS OFF" # Don't build DuckDB C++ tests, which we don't run anyway.
        "ENABLE_SANITIZER ${LOGSEARCH_ENABLE_ASAN}"
        "RELEASE_SANITIZER ${LOGSEARCH_ENABLE_ASAN}"
        "ENABLE_UBSAN ${LOGSEARCH_ENABLE_UBSAN}"
    )
  endblock()

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
