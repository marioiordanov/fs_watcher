#!/usr/bin/env bash
set -euo pipefail
clang -std=c11 -Wall -Wextra -O2 \
 -Ic/include \
 c/fs_watch.c c/watcher.c c/protocol.c c/util.c \
 -framework CoreServices -framework CoreFoundation \
 -o fs_watch
