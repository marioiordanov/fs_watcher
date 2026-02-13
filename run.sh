#!/usr/bin/env bash
set -euo pipefail

./build.sh
./fs_watch /Users/mario/Projects/test-watcher/data
