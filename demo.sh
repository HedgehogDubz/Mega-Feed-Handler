#!/bin/bash
# One command: build everything and run the test binary.
set -e
cd "$(dirname "$0")"
make -s

./bin/test
