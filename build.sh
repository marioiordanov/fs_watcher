#!/usr/bin/env bash
set -euo pipefail
clang -std=c11 -Wall -Wextra -O2 fs_watch.c -framework CoreServices -framework CoreFoundation -o fs_watch
