# SPDX-License-Identifier: GPL-3.0-or-later
# Radium Download Manager — Developer Convenience Wrapper
# Authoritative build orchestration is managed via CMake (CMakeLists.txt & CMakePresets.json).

BUILD_DIR ?= build
CONFIG    ?= Release

.PHONY: all config build test clean help

all: build

help:
	@echo "Radium Download Manager Build Targets:"
	@echo "  make config   - Configure project with CMake"
	@echo "  make build    - Build all targets via CMake"
	@echo "  make test     - Run CTest test suite"
	@echo "  make clean    - Remove build artifacts"

config:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)

build:
	@if [ ! -f $(BUILD_DIR)/CMakeCache.txt ]; then cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG); fi
	cmake --build $(BUILD_DIR) --config $(CONFIG)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -C $(CONFIG)

clean:
	cmake --build $(BUILD_DIR) --target clean 2>/dev/null || rm -rf $(BUILD_DIR)
