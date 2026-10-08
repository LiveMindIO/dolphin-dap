#!/bin/bash

# To lower build times, we avoid running the fetchDependencies script if the MoltenVK
# version didn't change. The last-built MoltenVK version is stored inside a file in
# the timestamp directory. If the file doesn't exist or the file contains a different
# MoltenVK version, fetchDependencies is ran.
#
# Usage: configure.sh <timestamp directory> <source directory> <MoltenVK version>
#

set -e

VERSION_PATH="$1/MoltenVK-last-version.txt"
CURRENT_VERSION="$3"
LAST_VERSION=$(cat "$VERSION_PATH" || true)

if ! [ "$LAST_VERSION" = "$3" ]; then
  # Dependency fetching can fail transiently (for example GitHub DNS on CI).
  # Never record success until every dependency has been fetched.
  for attempt in 1 2 3; do
    if "$2/fetchDependencies" --macos; then
      echo "$CURRENT_VERSION" > "$VERSION_PATH"
      exit 0
    fi
    if [ "$attempt" = 3 ]; then
      exit 1
    fi
    sleep 10
  done
fi
