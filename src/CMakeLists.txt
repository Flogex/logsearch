# The directory is added from the DuckDB source tree.
# DuckDB sets minimum_required itself, so we have to overwrite it to use the newest policies.
cmake_minimum_required(VERSION 3.28...4.4)

# Registers the extension targets using DuckDB's CMake helper functions.
# Component subdirectories add their sources with target_sources().
build_static_extension(logsearch logsearch_extension.cpp)
set(PARAMETERS " ")
build_loadable_extension(logsearch ${PARAMETERS} logsearch_extension.cpp)

# Each component subdirectory names its own unity group.
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

# Compiler warnings. PRIVATE so they don't propagate to DuckDB or consumers.
# Curated to be strict without being noisy. Warnings become errors only under ENABLE_LINTING.
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
  set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ${ENABLE_LINTING})
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
