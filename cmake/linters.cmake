include_guard(GLOBAL)

# Attaches clang-tidy and cppcheck to <target> so they run as part of the build. No-op unless
# LOGSEARCH_ENABLE_LINTING is set.
function(logsearch_apply_linters target)
  if(NOT LOGSEARCH_ENABLE_LINTING)
    return()
  endif()

  find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
  # CMAKE_CURRENT_SOURCE_DIR refers to caller's source directory
  set(CLANG_TIDY_CMD ${CLANG_TIDY_EXE} --header-filter=^${CMAKE_CURRENT_SOURCE_DIR}/)
  if(APPLE)
    # clang-tidy installed from Homebrew can't locate libc++ system headers without explicit -isysroot.
    execute_process(
      COMMAND xcrun --show-sdk-path
      OUTPUT_VARIABLE macos_sdk_path
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    list(APPEND CLANG_TIDY_CMD --extra-arg-before=-isysroot --extra-arg-before=${macos_sdk_path})
  endif()
  set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_CMD}")

  find_program(CPPCHECK_EXE NAMES cppcheck REQUIRED)
  # Sane defaults: skip `unusedFunction` (whole-program, slow) and
  # `missingInclude` (noisy on STL). `--error-exitcode=1` fails build on findings.
  set(
    CPPCHECK_CMD
    "${CPPCHECK_EXE};--enable=warning,performance,portability;--inline-suppr;--suppress=missingIncludeSystem;--suppress=*:*/_deps/*;--std=c++${CMAKE_CXX_STANDARD};--quiet;--error-exitcode=1"
  )
  set_target_properties(${target} PROPERTIES CXX_CPPCHECK "${CPPCHECK_CMD}")
endfunction()
