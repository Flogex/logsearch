set(
  DUCKDB_INDEX_SOURCE_FILES
  logsearch_index.cpp
  logsearch_index_builder.cpp
  logsearch_index_type.cpp
)

set_source_files_properties(
  ${DUCKDB_INDEX_SOURCE_FILES}
  TARGET_DIRECTORY ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME}
  PROPERTIES UNITY_GROUP duckdb-index
)

target_sources(${EXTENSION_NAME} PRIVATE ${DUCKDB_INDEX_SOURCE_FILES})
target_sources(${LOADABLE_EXTENSION_NAME} PRIVATE ${DUCKDB_INDEX_SOURCE_FILES})
