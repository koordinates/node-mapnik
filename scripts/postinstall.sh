#!/usr/bin/env bash
set -eu
set -o pipefail

# mapnik 4.x (Debian/trixie) installs input plugins under a version-numbered
# directory, e.g. /usr/lib/<arch>/mapnik/4.0/input. Older mapnik (3.x) used an
# unversioned /usr/lib/<arch>/mapnik/input. Detect whichever is present (the
# library is already installed at build time), preferring the versioned dir.
ARCH=$(uname -m)
INPUT_PLUGINS=$(ls -d "/usr/lib/${ARCH}-linux-gnu/mapnik/"*/input 2>/dev/null | sort -V | tail -n1 || true)
INPUT_PLUGINS=${INPUT_PLUGINS:-/usr/lib/${ARCH}-linux-gnu/mapnik/input}

SETTINGS="
var path = require('path');
module.exports.paths = {
    'fonts':         '/usr/share/fonts/truetype',
    'input_plugins': '${INPUT_PLUGINS}',
    'mapnik_index':  '$(which mapnik-index 2>/dev/null || true)',
    'shape_index':   '$(which shapeindex 2>/dev/null || true)'
};
module.exports.env = {
    'ICU_DATA':      '',
    'GDAL_DATA':     '$(pkg-config --variable=datadir gdal 2>/dev/null || echo /usr/share/gdal)',
    'PROJ_LIB':      '$(pkg-config --variable=datadir proj 2>/dev/null || echo /usr/share/proj)'
};
"

mkdir -p ./lib/binding
echo "$SETTINGS" > ./lib/binding/mapnik_settings.js

mkdir -p ./build/Release
echo "$SETTINGS" > ./build/Release/mapnik_settings.js
