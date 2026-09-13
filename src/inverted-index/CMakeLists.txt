set(
  INVERTED_INDEX_SOURCE_FILES
  inverted_index.cpp
  memtable.cpp
  postings_list.cpp
  ss_table.cpp
  ss_table_builder.cpp
)

set_source_files_properties(
  ${INVERTED_INDEX_SOURCE_FILES}
  TARGET_DIRECTORY ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME}
  PROPERTIES UNITY_GROUP inverted-index
)

target_sources(${EXTENSION_NAME} PRIVATE ${INVERTED_INDEX_SOURCE_FILES})
target_sources(${LOADABLE_EXTENSION_NAME} PRIVATE ${INVERTED_INDEX_SOURCE_FILES})
