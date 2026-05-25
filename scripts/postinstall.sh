#!/usr/bin/env bash
set -eu
set -o pipefail

SETTINGS="
var path = require('path');
module.exports.paths = {
    'fonts':         '/usr/share/fonts/truetype',
    'input_plugins': '/usr/lib/$(uname -m)-linux-gnu/mapnik/input',
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
