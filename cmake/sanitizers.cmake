include_guard(GLOBAL)

# Both compile and link options are PUBLIC so that anything linking the extension
# (viz. the unittest executable) is instrumented and links the sanitizer runtime.
function(logsearch_apply_sanitizers target)
  set(SANITIZER_FLAGS)
  if(LOGSEARCH_ENABLE_ASAN)
    list(APPEND SANITIZER_FLAGS -fsanitize=address)
  endif()
  if(LOGSEARCH_ENABLE_UBSAN)
    list(APPEND SANITIZER_FLAGS -fsanitize=undefined)
  endif()
  if(SANITIZER_FLAGS)
    target_compile_options(
      ${target}
      PUBLIC ${SANITIZER_FLAGS} -fno-sanitize-recover=all -fno-omit-frame-pointer
    )
    target_link_options(${target} PUBLIC ${SANITIZER_FLAGS})
  endif()
endfunction()
