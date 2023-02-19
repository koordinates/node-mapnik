# kx fork of node-mapnik

fork of https://github.com/mapnik/node-mapnik

Contains a Dockerfile which uses ubuntu focal deb dependencies, in order to use a GDAL version that supports curl.

# building and publishing a new package

* ensure all of these are in your env:
    - PATCH_VERSION_NUMBER (should be an integer, choose the next patch release version)
    - NPM_TOKEN
    - AWS_ACCESS_KEY_ID
    - AWS_SECRET_ACCESS_KEY
* `docker compose build --build-arg NODE_VERSION=12 mapnikbuilder`
* `docker-compose run mapnikbuilder`
* run `make` and `make publish-binary` and `make publish-npm` in the container

