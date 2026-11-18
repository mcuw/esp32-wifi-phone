#!/bin/sh
# Builds the three images for test_ota.py (issue #16):
#   build-ota-a  v0.0.1  autodial + OTA test hook   (flashed over USB)
#   build-ota-b  v0.0.2  OTA test hook              (offered first)
#   build-ota-c  v0.0.3  never marks itself valid   (must be rolled back)
# Needs sdkconfig.otatest.local with the manifest URL, see sdkconfig.otatest.
set -eu
cd "$(dirname "$0")/../.."
[ -f sdkconfig.otatest.local ] || { echo "sdkconfig.otatest.local missing, see sdkconfig.otatest" >&2; exit 1; }
IDF_PY=${IDF_PY:-idf.py}
build() {   # <dir> <version> <overlays>
    $IDF_PY -B "$1" -DSDKCONFIG="$1/sdkconfig" -DPROJECT_VER="$2" \
            -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;$3;sdkconfig.otatest.local" build
}
build build-ota-a v0.0.1 "sdkconfig.autodial;sdkconfig.otatest"
build build-ota-b v0.0.2 "sdkconfig.otatest"
build build-ota-c v0.0.3 "sdkconfig.otatest;sdkconfig.otatest.broken"
