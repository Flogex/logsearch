block(SCOPE_FOR VARIABLES)
  set(commit 6ee0826dcae55ed1e06b2c5701981221e979e1e6) # v3.15.0
  string(SUBSTRING "${commit}" 0 10 commit_short)
  CPMAddPackage(
    NAME Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG ${commit}
    GIT_SHALLOW TRUE
    CUSTOM_CACHE_KEY "${commit_short}/Catch2"
    SYSTEM YES
    EXCLUDE_FROM_ALL NO
  )
endblock()

add_executable(
  unittests
  test_basic.cpp
  analyzer/lowercaser.test.cpp
  analyzer/pipeline.test.cpp
  analyzer/tokenizer.test.cpp
  duckdb-index/logsearch_index.test.cpp
  inverted-index/inverted_index.test.cpp
  inverted-index/memtable.test.cpp
  inverted-index/ss_table.test.cpp
  storage/multi_block_writer.test.cpp
)

target_include_directories(unittests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(unittests PROPERTIES LINK_LIBRARIES_ONLY_TARGETS ON)

# DuckDB's FORCE_ASSERT option appends -DDUCKDB_FORCE_ASSERT to CMAKE_CXX_FLAGS in its own
# directory scope. Defining it again here keeps every target's view of D_ASSERT identical.
if(FORCE_ASSERT)
  target_compile_definitions(unittests PRIVATE DUCKDB_FORCE_ASSERT)
endif()

target_link_libraries(
  unittests
  PRIVATE
    # logsearch_extension pulls duckdb_static transitively (see build_static_extension).
    # Listing it explicitly here would double-list the .a and trip ld's "ignoring duplicate libraries" warning.
    # The extension's sanitizer options are PUBLIC, so they are inherited the same way.
    logsearch_extension
    # DuckDB registers its own and third_party include paths via the directory-scoped
    # include_directories() rather than as target-specific functions, so neither duckdb_static nor
    # logsearch_extension expose them through INTERFACE_INCLUDE_DIRECTORIES.
    # This is why src/ re-exports them as a separate interface library.
    logsearch_duckdb_headers
    duckdb_generated_extension_loader
    parquet_extension
    core_functions_extension
    Catch2::Catch2WithMain
)
