#!/bin/sh
# Builds and runs the PC test of the sketch. Needs g++ (Linux, macOS, or WSL on Windows).
# Usage:  ./run_tests.sh       (summary)
#         ./run_tests.sh -v    (also show the sketch's serial output)
set -e
cd "$(dirname "$0")"
g++ -std=gnu++11 -Wall -Imock -o test_charger test_charger.cpp
./test_charger "$@"
