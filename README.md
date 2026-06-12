# logsearch

[![CI](https://github.com/Flogex/logsearch/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Flogex/logsearch/actions/workflows/ci.yml)

DuckDB extension for full-text search, optimized for log data:

- Time-ordered, append-only, immutable
- Heavily skewed towards recent data
- Write-heavy
- Schemaless, wide, sparse
- Two query patterns: needle in a haystack and analytics

## Setup

**Required:**

- [just](https://github.com/casey/just) (`brew install just` or `cargo install just`)
- cmake ≥ 3.25
- ninja build generator
- pre-commit

**Recommended:**

- ccache or sccache

**Optional:**

- clang-tidy (used by linting recipes and the CI build)
- cppcheck (used by linting recipes and the CI build)

On macOS:

```sh
brew install bash just cmake ninja ccache llvm cppcheck pre-commit
```

`clang-tidy` ships with the Homebrew `llvm` formula; ensure it is on `PATH`.

Install the pre-commit hooks with:

```sh
pre-commit install
```

## Build

```sh
just            # default: build release
just [b]uild    # same
just build debug         # AddressSanitizer + UndefinedBehaviorSanitizer
```

Build artifacts are stored in `build/<preset>/`.

## Test

```sh
just test               # build release + run C++ unit tests
just test debug         # build debug + run unit tests under sanitizers
just test-sql           # run DuckDB SQLLogicTests filtered to logsearch
```

## Lint and format

```sh
just clang-tidy         # run clang-tidy on extension sources (uses release compile_commands.json)
just cppcheck           # run cppcheck on extension sources
just format             # run all pre-commit hooks
```
