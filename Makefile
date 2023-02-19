MODULE_NAME := $(shell node -e "console.log(require('./package.json').binary.module_name)")

SSE_MATH ?= true

default: release

ifneq (,$(findstring clang,$(CXX)))
    PROFILING_FLAG += -gline-tables-only
else
    PROFILING_FLAG += -g
endif

deps/geometry/include/mapbox/geometry.hpp:
	git submodule update --init

node_modules:
	npm install --ignore-scripts --clang

pre_build_check:
	@node -e "console.log('\033[94mNOTICE: to build from source you need mapnik >=',require('./package.json').mapnik_version,'\033[0m');"
	@echo "Looking for mapnik-config on your PATH..."
	mapnik-config -v

release_base: pre_build_check deps/geometry/include/mapbox/geometry.hpp node_modules
	V=1 CXXFLAGS="-fno-omit-frame-pointer $(PROFILING_FLAG)" ./node_modules/.bin/node-pre-gyp configure build --ENABLE_GLIBC_WORKAROUND=true --enable_sse=$(SSE_MATH) --loglevel=error --clang
	@echo "run 'make clean' for full rebuild"

debug_base: pre_build_check deps/geometry/include/mapbox/geometry.hpp node_modules
	V=1 ./node_modules/.bin/node-pre-gyp configure build --ENABLE_GLIBC_WORKAROUND=true --enable_sse=$(SSE_MATH) --loglevel=error --debug --clang
	@echo "run 'make clean' for full rebuild"

release:
	CXXFLAGS="-D_GLIBCXX_USE_CXX11_ABI=0" $(MAKE) release_base

debug:
	CXXFLAGS="-D_GLIBCXX_USE_CXX11_ABI=0" $(MAKE) debug_base

coverage:
	./scripts/coverage.sh

clean:
	rm -rf lib/binding
	rm -rf build
	rm -rf .mason
	find test/ -name *actual* -exec rm {} \;
	echo "run make distclean to also remove mason_packages and node_modules"

distclean: clean
	rm -rf node_modules
	rm -rf mason_packages

xcode: node_modules
	./node_modules/.bin/node-pre-gyp configure -- -f xcode

	@# If you need more targets, e.g. to run other npm scripts, duplicate the last line and change NPM_ARGUMENT
	SCHEME_NAME="$(MODULE_NAME)" SCHEME_TYPE=library BLUEPRINT_NAME=$(MODULE_NAME) BUILDABLE_NAME=$(MODULE_NAME).node scripts/create_scheme.sh
	SCHEME_NAME="npm test" SCHEME_TYPE=node BLUEPRINT_NAME=$(MODULE_NAME) BUILDABLE_NAME=$(MODULE_NAME).node NODE_ARGUMENT="`npm bin tape`/tape test/*.test.js" scripts/create_scheme.sh

	open build/binding.xcodeproj

docs:
	npm run docs

test:
	npm test

check: test

testpack:
	rm -f ./*tgz
	npm pack
	tar -ztvf *tgz
	rm -f ./*tgz

publish:
	npm version --git-tag-version=false "3.99.$(PATCH_VERSION_NUMBER)"
	./node_modules/node-pre-gyp/bin/node-pre-gyp package publish
	npm publish --access=restricted

.PHONY: test docs
