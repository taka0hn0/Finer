SHELL := /bin/zsh

BUILD_DIR := .build
MODULE_CACHE := $(BUILD_DIR)/swift-module-cache
C_HELPER := $(BUILD_DIR)/finder_ax_step
SWIFT_HELPER := $(BUILD_DIR)/finder_ax_move
ITERATIONS ?= 10
COUNTS ?= 10 1000 10000

.PHONY: all build check clean install uninstall benchmark-fixtures benchmark-column benchmark-list benchmark-icon benchmark-views test-finder-navigation

all: build

build: $(C_HELPER) $(SWIFT_HELPER)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR) $(MODULE_CACHE)

$(C_HELPER): src/finder_ax_step.c | $(BUILD_DIR)
	xcrun clang -std=c11 -O2 -Wall -Wextra -Werror \
		-framework ApplicationServices -framework Carbon \
		$< -o $@

$(SWIFT_HELPER): src/finder_ax_move.swift | $(BUILD_DIR)
	xcrun swiftc -O -module-cache-path $(MODULE_CACHE) \
		-framework AppKit -framework ApplicationServices \
		$< -o $@

check: build
	jq empty rules/generated/finder-vim.json
	zsh -n scripts/*.sh
	! rg -n '/Users/[^/]+/' . \
		--glob '!docs/FINDER_VIM_SPEC.md' \
		--glob '!README.md' \
		--glob '!Makefile' \
		--glob '!.build/**'

install: build
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

benchmark-fixtures:
	./scripts/prepare_benchmark_fixtures.sh

benchmark-column: benchmark-fixtures
	FINDER_VIM_BENCHMARK_COUNTS="$(COUNTS)" \
		./scripts/benchmark_column_jlj.sh "$(ITERATIONS)"

benchmark-list: benchmark-fixtures
	FINDER_VIM_BENCHMARK_COUNTS="$(COUNTS)" \
		./scripts/benchmark_view_navigation.sh list "$(ITERATIONS)"

benchmark-icon: benchmark-fixtures
	FINDER_VIM_BENCHMARK_COUNTS="$(COUNTS)" \
		./scripts/benchmark_view_navigation.sh icon "$(ITERATIONS)"

benchmark-views: benchmark-list benchmark-column benchmark-icon

test-finder-navigation: benchmark-fixtures
	./scripts/test_finder_navigation.sh

clean:
	rm -rf $(BUILD_DIR)
