# The directory is added from the DuckDB source tree.
# DuckDB sets minimum_required itself, so we have to overwrite it to use the newest policies.
cmake_minimum_required(VERSION 3.28...4.4)

set(
  SOURCE_FILES
  analyzer/analyzer_ub.cpp
  duckdb-index/duckdb_index_ub.cpp
  inverted-index/inverted_index_ub.cpp
  storage/multi_block_writer.cpp
  logsearch_extension.cpp
)

# Registers the extension targets using DuckDB's CMake helper functions
build_static_extension(logsearch ${SOURCE_FILES})
set(PARAMETERS " ")
build_loadable_extension(logsearch ${PARAMETERS} ${SOURCE_FILES})

target_include_directories(${EXTENSION_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Strict compiler warnings on our own targets. The curated list is shared with the
# unit test executable via cmake/warnings.cmake so the two stay in sync.
foreach(target IN ITEMS ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  logsearch_apply_warnings(${target})
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

# Hardening flags per OpenSSF Compiler Options Hardening Guide for C/C++:
# https://best.openssf.org/Compiler-Hardening-Guides/Compiler-Options-Hardening-Guide-for-C-and-C++.html
#
# Applied PRIVATE on extension targets only. Setting them at top level via
# CMAKE_CXX_FLAGS_* would also flow into the DuckDB subdir build, which has its own
# warning policy and would fail under -Wformat=2 / -Werror=format-security.
# Most flags here only affect the TU being compiled (defines, stack canary, format
# warnings), so per-target scope is sufficient for our code paths.
#
# Skipped on purpose:
#   - -fPIE / -pie: applies to executables. Loadable extension is a shared library
#     (already built -fPIC by build_loadable_extension); PIE belongs on the host binary.
#   - -Wl,-z,relro / -z,now / -z,noexecstack / -z,nodlopen: linker hardening of the
#     final binary. The host (duckdb CLI / process) decides, not a dlopen'd .so.
#   - -fhardened (GCC 14+): convenient bundle, but implies PIE — not applicable here.
#   - SafeStack (-fsanitize=safe-stack, clang): requires the safe/unsafe-stack ABI to
#     match across the whole process. Extension is dlopen'd into a host built without
#     it, so unilateral opt-in causes runtime mismatches. Also marked experimental.
#   - ShadowCallStack (-fsanitize=shadow-call-stack, clang AArch64/RISC-V): reserves
#     x18 process-wide and needs every loaded module compiled with -ffixed-x18. Same
#     blocker — we don't control the host binary.
include(CheckCXXCompilerFlag)
include(CMakePushCheckState)

# Promote "argument unused during compilation" to an error during probing so flags
# the driver accepts but the backend doesn't implement (e.g. -fstack-clash-protection
# on Apple aarch64 clang) are reported unsupported. Otherwise check_cxx_compiler_flag
# returns true and every TU emits a -Wunused-command-line-argument warning.
cmake_push_check_state()
check_cxx_compiler_flag(-Werror=unused-command-line-argument HAVE_WERROR_UNUSED_ARG)
if(HAVE_WERROR_UNUSED_ARG)
  list(APPEND CMAKE_REQUIRED_FLAGS -Werror=unused-command-line-argument)
endif()

set(LOGSEARCH_HARDENING_FLAGS)
foreach(
  flag
  IN
  ITEMS
    -fstack-protector-strong
    -fstack-clash-protection
    -fcf-protection=full
    -fno-delete-null-pointer-checks
    -fno-strict-overflow
    -ftrivial-auto-var-init=zero
    -fstrict-flex-arrays=3
    -Wformat
    -Wformat=2
    -Werror=format-security
    -Wimplicit-fallthrough
    -Wtrampolines
    -mbranch-protection=standard
)
  string(MAKE_C_IDENTIFIER "HAVE_FLAG_${flag}" flag_var)
  check_cxx_compiler_flag("${flag}" ${flag_var})
  if(${flag_var})
    list(APPEND LOGSEARCH_HARDENING_FLAGS "${flag}")
  endif()
endforeach()
cmake_pop_check_state()

# _FORTIFY_SOURCE needs optimization (>= -O1) and conflicts with ASan/UBSan runtime
# instrumentation. Gate on non-Debug and no sanitizers. -U guards against an existing
# definition from upstream / system flags causing a redefinition warning.
set(
  LOGSEARCH_FORTIFY_GUARD
  "$<AND:$<NOT:$<CONFIG:Debug>>,$<NOT:$<BOOL:${LOGSEARCH_ENABLE_ASAN}>>,$<NOT:$<BOOL:${LOGSEARCH_ENABLE_UBSAN}>>>"
)

foreach(target IN ITEMS ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
  target_compile_options(
    ${target}
    PRIVATE
      ${LOGSEARCH_HARDENING_FLAGS}
      "$<${LOGSEARCH_FORTIFY_GUARD}:SHELL:-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3>"
  )
  # libstdc++ bounds checks. Harmless on libc++ (macOS) where the macro is unused.
  target_compile_definitions(${target} PRIVATE _GLIBCXX_ASSERTIONS)
endforeach()

logsearch_apply_linters(${EXTENSION_NAME})
