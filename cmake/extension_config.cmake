include_guard(GLOBAL)

cmake_path(SET LOGSEARCH_ROOT NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/..")
# SOURCE_DIR is the directory DuckDB calls add_subdirectory() on.
# It is set to src/ because only the extension targets belong in DuckDB's build.
# TEST_DIR is set explicitly because otherwise it defaults to ${SOURCE_DIR}/test/sql.
duckdb_extension_load(logsearch
    SOURCE_DIR ${LOGSEARCH_ROOT}/src
    INCLUDE_DIR ${LOGSEARCH_ROOT}/src
    TEST_DIR ${LOGSEARCH_ROOT}/test/sql
    LOAD_TESTS
)
