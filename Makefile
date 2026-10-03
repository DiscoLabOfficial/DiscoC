# Convenience entry points; CMake remains the incremental build system.
# Run with GNU Make and Bash (Linux/macOS, or MSYS2/Git Bash on Windows).
SHELL := bash
BACKEND ?= make
CONFIG ?= Release
JOBS ?= 4
BUILD_DIR ?= build/$(BACKEND)-$(CONFIG)
DIRECT_BUILD_DIR ?= build/direct-$(CONFIG)
ROOT := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

.PHONY: all test direct direct-test help
all:
	bash "$(ROOT)build.sh" --backend "$(BACKEND)" --config "$(CONFIG)" --build-dir "$(BUILD_DIR)" --jobs "$(JOBS)"
test:
	bash "$(ROOT)build.sh" --backend "$(BACKEND)" --config "$(CONFIG)" --build-dir "$(BUILD_DIR)" --jobs "$(JOBS)" --test
direct:
	bash "$(ROOT)build.sh" --backend direct --config "$(CONFIG)" --build-dir "$(DIRECT_BUILD_DIR)"
direct-test:
	bash "$(ROOT)build.sh" --backend direct --config "$(CONFIG)" --build-dir "$(DIRECT_BUILD_DIR)" --test
help:
	bash "$(ROOT)build.sh" --help
