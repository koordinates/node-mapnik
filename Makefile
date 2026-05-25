MODULE_NAME := $(shell node -e "console.log(require('./package.json').binary.module_name)")

ifeq ($(shell uname -p), arm)
    auto_enable_sse=false
else
    auto_enable_sse=true
endif

SSE_MATH ?= $(auto_enable_sse)

default: release

ifneq (,$(findstring clang,$(CXX)))
    PROFILING_FLAG += -gline-tables-only
else
    PROFILING_FLAG += -g
endif

deps/geometry/include/mapbox/geometry.hpp:
	git submodule update --init

node_modules/.package-lock.json:
	npm install --ignore-scripts

pre_build_check:
	@node -e "console.log('\033[94mNOTICE: to build from source you need mapnik >=',require('./package.json').mapnik_version,'\033[0m');"
	@echo "Looking for pkg-config on your PATH..."
	pkg-config libmapnik --modversion

release_base: pre_build_check deps/geometry/include/mapbox/geometry.hpp node_modules/.package-lock.json
	V=1 CXXFLAGS="-fno-omit-frame-pointer $(PROFILING_FLAG)" npx node-gyp configure build --ENABLE_GLIBC_WORKAROUND=true --enable_sse=$(SSE_MATH) --loglevel=error --clang
	./scripts/postinstall.sh
	rm -f lib/binding/mapnik.node
	cp build/Release/mapnik.node lib/binding/
	@echo "run 'make clean' for full rebuild"

debug_base: pre_build_check deps/geometry/include/mapbox/geometry.hpp node_modules/.package-lock.json
	V=1 npx node-gyp configure build --ENABLE_GLIBC_WORKAROUND=true --enable_sse=$(SSE_MATH) --loglevel=error --debug --clang
	@echo "run 'make clean' for full rebuild"

release:
	$(MAKE) release_base

debug:
	$(MAKE) debug_base

coverage:
	./scripts/coverage.sh

tidy:
	./scripts/clang-tidy.sh

format:
	./scripts/clang-format.sh

sanitize:
	./scripts/sanitize.sh

clean:
	rm -rf lib/binding
	rm -rf build
	rm -rf ./.mason
	# remove remains from running 'make coverage'
	rm -f *.profraw
	rm -f *.profdata
	find test/ -name "*actual*" -exec rm {} \;
	echo "run make distclean to also remove mason_packages and node_modules"

distclean: clean
	rm -rf ./.toolchain
	rm -rf node_modules
	rm -rf mason_packages
	# remove remains from running './scripts/setup.sh'
	rm -rf .mason
	rm -rf .toolchain
	rm -f local.env

xcode: node_modules/.package-lock.json
	npx node-gyp configure -- -f xcode

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

publish-binary:
	npm version --git-tag-version=false --allow-same-version "4.99.$(PATCH_VERSION_NUMBER)"
	echo "aws token is $(AWS_ACCESS_KEY_ID)"
	aws sts get-caller-identity
	npx node-pre-gyp package publish

publish-npm:
	npm version --git-tag-version=false --allow-same-version "4.99.$(PATCH_VERSION_NUMBER)"
	npm publish --access=public

.PHONY: test docs
