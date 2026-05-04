#!/usr/bin/env bash
set -euo pipefail

status=0
for f in "$@"; do
    if ! grep -q "^#pragma once" "$f"; then
        echo "Missing '#pragma once': $f" >&2
        status=1
    fi
done
exit "$status"
