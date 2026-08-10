include_guard(GLOBAL)

# Curated compiler warnings shared by logsearch's own targets: the extension
# (static + loadable) and the unit test executable. Kept in one place so the
# extension and the tests stay in sync.
#
# Applied PRIVATE so they never propagate to DuckDB or consumers. Warnings become
# errors only under ENABLE_WERROR (implied by ENABLE_LINTING) so local dev builds
# still succeed on new warnings while CI treats them as errors.
function(logsearch_apply_warnings target)
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
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC-only warnings (Clang doesn't accept some of these or treats them as no-ops).
    # -Wno-missing-field-initializers: GCC fires this on C++20 designated initializers
    # that omit fields (relying on default member init).
    list(
      APPEND WARN_FLAGS
      -Wmisleading-indentation
      -Wduplicated-cond
      -Wduplicated-branches
      -Wlogical-op
      -Wno-missing-field-initializers
    )
  endif()
  target_compile_options(${target} PRIVATE ${WARN_FLAGS})
  if(ENABLE_WERROR OR ENABLE_LINTING)
    set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
  endif()
endfunction()
