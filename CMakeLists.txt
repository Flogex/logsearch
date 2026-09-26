cmake_minimum_required(VERSION 3.28...4.4)
project(LogsearchExtension CXX)

# The root CMakeLists.txt is absent in DuckDB-driven builds, so anything declared there must
# tolerate being undeclared.
option(LOGSEARCH_ENABLE_LINTING "Run clang-tidy and cppcheck on extension targets during build" OFF)
option(LOGSEARCH_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(LOGSEARCH_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
  message(
    WARNING
    "Using a compiler cache like (s)ccache is recommended. Configure one by exporting the "
    "CMAKE_CXX_COMPILER_LAUNCHER environment variable. Pass -DCMAKE_CXX_COMPILER_LAUNCHER= "
    "to silence this warning."
  )
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
set(FETCHCONTENT_QUIET OFF)

# File downloaded from https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.43.1/CPM.cmake
include(${CMAKE_CURRENT_LIST_DIR}/cmake/CPM.cmake)

# Tell DuckDB to bake logsearch into duckdb_static and register its SQLLogicTests so the duckdb
# unittest binary can resolve `require logsearch`. DuckDB consumes the extension config during its
# own CMake processing and calls add_subdirectory() on src/, which is where the extension targets
# are defined.
set(
  DUCKDB_EXTENSION_CONFIGS
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/extension_config.cmake"
  CACHE STRING
  ""
  FORCE
)
# Using CPM defaults policies CMP0077/0126/0135/0150 to NEW for every subproject, which is newer
# than 3.14...3.29 that DuckDB requests. But that doesn't break anything (yet).
block(SCOPE_FOR VARIABLES)
  set(commit d8a1bd4f4fbccdf3d22d8f1fe27a94c11c805d0a) # main as of 2026-09-25
  string(SUBSTRING "${commit}" 0 10 commit_short)
  CPMAddPackage(
    NAME duckdb
    GIT_REPOSITORY https://github.com/duckdb/duckdb.git
    GIT_TAG ${commit}
    # `GIT_SHALLOW TRUE` only works when the pinned commit is a branch tip or tagged.
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
