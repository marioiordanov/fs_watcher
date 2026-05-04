#!/usr/bin/env bash
clang -Wall -Wextra -DRUN_TESTS -O2 \
 -Ic/ \
 c/fs_watch.c c/watcher.c c/protocol.c c/util.c \
 -framework CoreServices -framework CoreFoundation \
 -o tests && ./tests

rm ./tests
