set shell := ["bash", "-cu"]

proj_dir := justfile_directory()
build_dir := proj_dir / "build"
extra_flags := if env('OSX_BUILD_ARCH', '') != '' { '-DOSX_BUILD_ARCH=' + env('OSX_BUILD_ARCH') } else { '' }

# Configure CMake for a preset. Skipped if CMakeCache.txt already present.
[group("build")]
configure preset="release":
    #!/usr/bin/env bash
    if [ ! -f "{{build_dir}}/{{preset}}/CMakeCache.txt" ]; then
        cmake --preset {{preset}} {{extra_flags}}
    fi

# Build any preset (release/debug/reldebinfo). Configures CMake if needed.
[group("build")]
[default]
build preset="release": (configure preset)
    cmake --build --preset {{preset}}

# Force CMake reconfigure for a preset, then build.
[group("build")]
rebuild preset="release":
    rm -f {{build_dir}}/{{preset}}/CMakeCache.txt
    @just build {{preset}}

# Run C++ unit tests for the given preset (release/debug/reldebinfo). Trailing args are forwarded to the Catch2
# binary: a tag like '[inverted_index]', a test-name pattern, or flags like -s. Example: `just test release "[bug]"`.
[group("test")]
test preset="release" *args: (build preset)
    #!/usr/bin/env bash
    set -euf -o pipefail
    {{build_dir}}/{{preset}}/test/unittests {{args}}

# Run SQLLogicTests for the logsearch extension.
[group("test")]
test-sqllogic: (build "release")
    {{build_dir}}/release/_deps/duckdb-build/test/unittest "*logsearch/test/sql/*"

# Build with Clang source-based coverage, run the unit tests, and report line/branch coverage of src/.
# Prints a summary table, writes an HTML report and an LCOV trace to build/coverage/coverage-report/.
# Trailing args are forwarded to the Catch2 binary, e.g. `just coverage '[analyzer]'`.
[group("test")]
coverage *args: (build "coverage")
    #!/usr/bin/env bash
    # No -f here (unlike the test recipe): the profraw glob below needs pathname expansion.
    set -eu -o pipefail
    bin={{build_dir}}/coverage/test/unittests
    out={{build_dir}}/coverage/coverage-report
    rm -rf "$out"
    mkdir -p "$out"
    # llvm-profdata/llvm-cov must be at least as new as the clang that produced the profile.
    # The coverage preset uses the system clang, so use the matching toolchain tools.
    if [[ "$OSTYPE" == "darwin"* ]]; then
        llvm() { xcrun "$@"; }
    else
        llvm() { "$@"; }
    fi
    LLVM_PROFILE_FILE="$out/unittests-%p.profraw" "$bin" {{args}}
    llvm llvm-profdata merge -sparse "$out"/*.profraw -o "$out/unittests.profdata"
    # Restricting to src/ drops DuckDB, Catch2, and the test sources from the report.
    llvm llvm-cov report "$bin" -instr-profile="$out/unittests.profdata" {{proj_dir}}/src
    llvm llvm-cov show "$bin" -instr-profile="$out/unittests.profdata" \
        -format=html -output-dir="$out/html" -show-branches=count {{proj_dir}}/src
    llvm llvm-cov export "$bin" -instr-profile="$out/unittests.profdata" \
        -format=lcov {{proj_dir}}/src > "$out/coverage.lcov"
    echo
    echo "HTML report: $out/html/index.html"
    echo "LCOV trace:  $out/coverage.lcov"

# Run clang-tidy on extension sources without building.
[group("lint")]
clang-tidy preset="release": (configure preset)
    #!/usr/bin/env bash
    set -euo pipefail
    extra=()
    if [[ "$OSTYPE" == "darwin"* ]]; then
        # clang-tidy installed from Homebrew can't locate libc++ system headers without explicit -isysroot.
        extra+=(--extra-arg-before=-isysroot --extra-arg-before="$(xcrun --show-sdk-path)")
    fi
    clang-tidy -p {{build_dir}}/{{preset}} --header-filter=^{{proj_dir}}/src/ "${extra[@]}" {{proj_dir}}/src/*.cpp

# Run cppcheck on extension sources without building.
[group("lint")]
cppcheck preset="release": (configure preset)
    cppcheck \
        --project={{build_dir}}/{{preset}}/compile_commands.json \
        --file-filter='{{proj_dir}}/src/*' \
        --enable=warning,performance,portability \
        --inline-suppr \
        --suppress=missingIncludeSystem \
        --suppress='*:*/_deps/*' \
        --std=c++20 \
        --quiet \
        --error-exitcode=1

# Run all pre-commit hooks.
format:
    pre-commit run --all-files

# Remove the build directory. Optional preset arg restricts removal to build/<preset> only (e.g. `just clean debug`).
clean preset="":
    #!/usr/bin/env bash
    if [ -n "{{preset}}" ]; then
        rm -rf "{{build_dir}}/{{preset}}"
    else
        rm -rf "{{build_dir}}"
    fi

alias b := build
alias t := test
