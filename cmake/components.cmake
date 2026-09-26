include_guard(GLOBAL)

# Adds a component's sources to the extension target.
# With UNITY the sources form one unity group named <name>, otherwise they become separate
# translation units.
# Source paths are relative to the calling CMakeLists.
function(logsearch_add_component name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "UNITY" "" "SOURCES")
  if(arg_UNPARSED_ARGUMENTS)
    message(
      FATAL_ERROR
      "logsearch_add_component(${name}): unexpected arguments '${arg_UNPARSED_ARGUMENTS}'."
    )
  endif()
  if(NOT arg_SOURCES)
    message(FATAL_ERROR "logsearch_add_component(${name}): SOURCES is required")
  endif()

  if(arg_UNITY)
    set_source_files_properties(
      ${arg_SOURCES}
      TARGET_DIRECTORY ${EXTENSION_NAME}
      PROPERTIES UNITY_GROUP ${name}
    )
  endif()

  target_sources(${EXTENSION_NAME} PRIVATE ${arg_SOURCES})
endfunction()
