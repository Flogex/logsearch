set(ANALYZER_SOURCE_FILES lowercaser.cpp pipeline.cpp term_length_guard.cpp tokenizer.cpp)

set_source_files_properties(
  ${ANALYZER_SOURCE_FILES}
  TARGET_DIRECTORY ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME}
  PROPERTIES UNITY_GROUP analyzer
)

target_sources(${EXTENSION_NAME} PRIVATE ${ANALYZER_SOURCE_FILES})
target_sources(${LOADABLE_EXTENSION_NAME} PRIVATE ${ANALYZER_SOURCE_FILES})
