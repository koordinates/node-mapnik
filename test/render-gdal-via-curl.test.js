"use strict";

var mapnik = require('../');
var assert = require('assert');
var path = require('path');
var exists = require('fs').existsSync || require('path').existsSync;

mapnik.register_datasource(path.join(mapnik.settings.paths.input_plugins, 'shape.input'));
mapnik.register_datasource(path.join(mapnik.settings.paths.input_plugins, 'gdal.input'));

describe('mapnik rendering gdal with curl ', function () {
    it('should render gdal vsicurl datasource', function (done) {
        var filename = './test/tmp/renderVsiCurl.png';
        var map = new mapnik.Map(600, 400);
        map.loadSync('./test/raster-vsicurl.xml');
        map.zoomAll();
        map.renderFile(filename, function (error) {
            console.log(error)
            assert.ok(!error);
            assert.ok(exists(filename));
            done();
        });
    });

});
