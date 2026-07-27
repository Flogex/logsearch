include_guard(GLOBAL)

cmake_path(SET LOGSEARCH_ROOT NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/..")
duckdb_extension_load(logsearch
    SOURCE_DIR ${LOGSEARCH_ROOT}
    INCLUDE_DIR ${LOGSEARCH_ROOT}/src
    LOAD_TESTS
)
