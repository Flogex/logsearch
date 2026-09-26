# The directory is added from the DuckDB source tree.
# In a DuckDB-driven build, our root CMakeLists.txt is never called.
# Hence, the extension targets have to be set up entirely from here.
# DuckDB sets minimum_required itself, so we have to overwrite it to use the newest policies.
cmake_minimum_required(VERSION 3.28...4.4)

# We provide our own option to control unity builds instead of relying on CMAKE_UNITY_BUILD.
# CMAKE_UNITY_BUILD initializes UNITY_BUILD on every target, but we want a separate knob for
# Logsearch. Because it defaults to ON it has to be defined in src/ instead of the root CMakeLists,
# otherwise DuckDB-driven builds wouldn't set it.
option(LOGSEARCH_UNITY_BUILD "Compile subcomponents as one translation unit each" ON)

set(EXTENSION_NAME logsearch_extension)
set(LOADABLE_EXTENSION_NAME logsearch_loadable_extension)

# A unity build's compilation database names generated unity sources rather than our own files, so
# cppcheck's file filter skips them and clangd and iwyu_tool.py cannot attribute anything to a
# single source.
if(LOGSEARCH_ENABLE_LINTING AND LOGSEARCH_UNITY_BUILD)
  message(
    FATAL_ERROR
    "LOGSEARCH_ENABLE_LINTING requires LOGSEARCH_UNITY_BUILD=OFF. Pass -DLOGSEARCH_UNITY_BUILD=OFF "
    "or use the release-lint preset."
  )
endif()

cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH LOGSEARCH_ROOT)
include(${LOGSEARCH_ROOT}/cmake/components.cmake)
include(${LOGSEARCH_ROOT}/cmake/linters.cmake)
include(${LOGSEARCH_ROOT}/cmake/sanitizers.cmake)

# DuckDB uses old-style include_directories(). Hence, we have a bunch of directories in
# INCLUDE_DIRECTORIES now that are added with -I, not -isystem, to the compiler arguments. This
# means we will get compiler warnings from DuckDB's headers. To avoid that, we re-add DuckDB's
# include directories as SYSTEM.
# logsearch_duckdb_headers republishes them as a target so consumers outside this tree (test/)
# can link against them instead of reading the directory property back off DuckDB's source directory.
# It is deliberately not linked into the extension targets because those go into DuckDB's export
# set. It would force us to publish this internal build target as well.
block(SCOPE_FOR VARIABLES)
  get_directory_property(inherited_includes INCLUDE_DIRECTORIES)
  add_library(logsearch_duckdb_headers INTERFACE)
  if(inherited_includes)
    set_directory_properties(PROPERTIES INCLUDE_DIRECTORIES "")
    foreach(dir IN LISTS inherited_includes)
      include_directories(SYSTEM ${dir})
    endforeach()
    target_include_directories(logsearch_duckdb_headers SYSTEM INTERFACE ${inherited_includes})
  endif()
endblock()

# Registers the extension targets using DuckDB's CMake helper functions.
# Component subdirectories add their sources with target_sources().
build_static_extension(logsearch logsearch_extension.cpp)
build_loadable_extension(logsearch "" logsearch_extension.cpp)

# Components name their unity group, if any, via logsearch_add_component().
foreach(target IN ITEMS ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  set_target_properties(
    ${target}
    PROPERTIES UNITY_BUILD ${LOGSEARCH_UNITY_BUILD} UNITY_BUILD_MODE GROUP
  )
endforeach()

add_subdirectory(analyzer)
add_subdirectory(duckdb-index)
add_subdirectory(inverted-index)
add_subdirectory(scalars)
add_subdirectory(storage)

# Both targets compile the same sources, which would put two entries per file in
# compile_commands.json and make clangd and iwyu_tool.py pick one arbitrarily or run twice.
set_target_properties(${LOADABLE_EXTENSION_NAME} PROPERTIES EXPORT_COMPILE_COMMANDS OFF)

# The static extension is installed into DuckDB's export set below, which rejects source-tree paths.
# BUILD_INTERFACE keeps the path for builds here and drops it in external installs.
target_include_directories(${EXTENSION_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# DuckDB is added with SYSTEM from the root, and this directory sits inside its tree, so the
# extension inherits this property. However, we don't want outside consumers (unittests) to consume
# our own headers with -isystem.
set_target_properties(${EXTENSION_NAME} PROPERTIES SYSTEM OFF)

# Compiler warnings. PRIVATE so they don't propagate to DuckDB or consumers.
# Curated to be strict without being noisy. Warnings become errors only under
# LOGSEARCH_ENABLE_LINTING.
set(
  WARN_FLAGS
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wnon-virtual-dtor
  -Wold-style-cast
  -Wcast-align
  -Woverloaded-virtual
  -Wnull-dereference
  -Wdouble-promotion
  -Wformat=2
  -Wimplicit-fallthrough
  -Wextra-semi
  -Wunused
  -Wzero-as-null-pointer-constant
)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Enforces the DUCKDB_GUARDED_BY / DUCKDB_REQUIRES annotations.
  list(APPEND WARN_FLAGS -Wthread-safety)
  # For clang::nonblocking et al. attributes
  list(APPEND WARN_FLAGS -Wfunction-effects)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC-only warnings (Clang doesn't accept some of these or treats them as no-ops).
  # -Wno-missing-field-initializers: GCC fires this on C++20 designated initializers
  # that omit fields (relying on default member init)
  list(
    APPEND WARN_FLAGS
    -Wmisleading-indentation
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wno-missing-field-initializers
  )
endif()
foreach(target IN ITEMS ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  target_compile_options(${target} PRIVATE ${WARN_FLAGS})
  if(LOGSEARCH_ENABLE_LINTING)
    set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
  endif()
endforeach()

# Expose the static extension to DuckDB's install set so packaging /
# distribution flows pick it up. DUCKDB_EXPORT_SET and INSTALL_LIB_DIR are
# defined by DuckDB's CMake when it configures extensions.
install(
  TARGETS ${EXTENSION_NAME}
  EXPORT ${DUCKDB_EXPORT_SET}
  LIBRARY DESTINATION ${INSTALL_LIB_DIR}
  ARCHIVE DESTINATION ${INSTALL_LIB_DIR}
)

foreach(target IN ITEMS ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  logsearch_apply_sanitizers(${target})
endforeach()

logsearch_apply_linters(${EXTENSION_NAME})
