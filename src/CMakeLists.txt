set(SOURCE_FILES logsearch_extension.cpp)

build_static_extension(logsearch ${SOURCE_FILES})
set(PARAMETERS " ")
build_loadable_extension(logsearch ${PARAMETERS} ${SOURCE_FILES})

target_include_directories(${EXTENSION_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Expose the static extension to DuckDB's install set so packaging /
# distribution flows pick it up. DUCKDB_EXPORT_SET and INSTALL_LIB_DIR are
# defined by DuckDB's CMake when it configures extensions.
install(
  TARGETS ${EXTENSION_NAME}
  EXPORT ${DUCKDB_EXPORT_SET}
  LIBRARY DESTINATION ${INSTALL_LIB_DIR}
  ARCHIVE DESTINATION ${INSTALL_LIB_DIR}
)

set(SANITIZER_FLAGS)
if(ENABLE_ASAN)
  list(APPEND SANITIZER_FLAGS -fsanitize=address)
endif()
if(ENABLE_UBSAN)
  list(APPEND SANITIZER_FLAGS -fsanitize=undefined)
endif()
if(SANITIZER_FLAGS)
  foreach(target IN ITEMS ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
    # Compile flags are PRIVATE to avoid propagating -fsanitize=... to DuckDB itself.
    # We only want to instrument our extension code.
    target_compile_options(
      ${target}
      PRIVATE ${SANITIZER_FLAGS} -fno-sanitize-recover=all -fno-omit-frame-pointer
    )
    # Link flags are propagates to consumers (e.g. duckdb_static) so the final binary links the sanitizer runtime.
    # Otherwise, we would get unresolved __asan_/__ubsan_ symbols.
    target_link_options(${target} PUBLIC ${SANITIZER_FLAGS})
  endforeach()
endif()

if(ENABLE_LINTING)
  # Sources are identical to the loadable target, so running sanitizers on both would duplicate work.
  find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
  set(CLANG_TIDY_CMD ${CLANG_TIDY_EXE} --header-filter=^${CMAKE_CURRENT_SOURCE_DIR}/)
  if(APPLE)
    # clang-tidy installed from Homebrew can't locate libc++ system headers without explicit -isysroot.
    execute_process(
      COMMAND xcrun --show-sdk-path
      OUTPUT_VARIABLE MACOS_SDK_PATH
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    list(APPEND CLANG_TIDY_CMD --extra-arg-before=-isysroot --extra-arg-before=${MACOS_SDK_PATH})
  endif()
  set_target_properties(${EXTENSION_NAME} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_CMD}")

  find_program(CPPCHECK_EXE NAMES cppcheck REQUIRED)
  # Sane defaults: skip `unusedFunction` (whole-program, slow) and
  # `missingInclude` (noisy on STL). `--error-exitcode=1` fails build on findings.
  set(
    CPPCHECK_CMD
    "${CPPCHECK_EXE};--enable=warning,performance,portability;--inline-suppr;--suppress=missingIncludeSystem;--suppress=*:*/_deps/*;--std=c++${CMAKE_CXX_STANDARD};--quiet;--error-exitcode=1"
  )
  set_target_properties(${EXTENSION_NAME} PROPERTIES CXX_CPPCHECK "${CPPCHECK_CMD}")
endif()
