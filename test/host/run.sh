#!/bin/sh
# Host-side unit tests for the pure-logic parts of the firmware (no ESP-IDF needed).
set -eu
cd "$(dirname "$0")"
SRC=../../main
CC=${CC:-cc}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
FLAGS="-std=gnu11 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all"

$CC $FLAGS -I$SRC adpcm_test.c $SRC/adpcm.c       -lm -o "$OUT/adpcm_test"
$CC $FLAGS -I$SRC json_test.c  $SRC/json_util.c       -o "$OUT/json_test"
$CC $FLAGS -Istub -I$SRC dns_test.c $SRC/dns_server.c -o "$OUT/dns_test"

"$OUT/adpcm_test"
"$OUT/json_test"
"$OUT/dns_test"
echo "all host tests passed"
