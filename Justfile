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

# Run C++ unit tests for the given preset (release/debug/reldebinfo).
[group("test")]
test preset="release": (build preset)
    {{build_dir}}/{{preset}}/test/unittests

# Run SQLLogicTests for the logsearch extension.
[group("test")]
test-sqllogic: (build "release")
    {{build_dir}}/release/_deps/duckdb-build/test/unittest "*logsearch/test/sql/*"

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
