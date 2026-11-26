#!/bin/sh
# Builds modules/swmongo.so. Re-run after editing library/*.php (they are embedded in the .so).
set -e
cd "$(dirname "$0")"
php tools/gen_library.php
phpize
./configure --enable-swmongo "$@"
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
echo "Built: $(pwd)/modules/swmongo.so"
