SHELL := /bin/zsh

BUILD_DIR := .build
MODULE_CACHE := $(BUILD_DIR)/swift-module-cache
C_HELPER := $(BUILD_DIR)/finder_ax_step
SWIFT_HELPER := $(BUILD_DIR)/finder_ax_move
VISUAL_CAPTURE_HELPER := $(BUILD_DIR)/finer_visual_capture
ITERATIONS ?= 10
COUNTS ?= 10 1000 10000
BASELINE_REF ?= 793a82c
CANDIDATE_REF ?= HEAD
VERSION ?=

.PHONY: all build rules check-rules check clean install uninstall test-install dist test-dist benchmark-comparison-helpers benchmark-fixtures benchmark-realistic-fixtures benchmark-column benchmark-list benchmark-icon benchmark-views benchmark-column-realistic benchmark-list-realistic benchmark-icon-realistic benchmark-realistic-views benchmark-worker-timeout benchmark-hold benchmark-hold-realistic benchmark-hold-preflight benchmark-hold-realistic-preflight benchmark-taps benchmark-taps-realistic benchmark-taps-preflight benchmark-taps-realistic-preflight benchmark-visual-helper benchmark-column-visual benchmark-column-visual-realistic test-visual-latency-analyzer test-finder-navigation test-finder-selection

all: build

build: $(C_HELPER) $(SWIFT_HELPER)

rules:
	./scripts/generate_rules.sh

check-rules:
	./scripts/generate_rules.sh --check
	./scripts/test_rule_generation.sh

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

$(VISUAL_CAPTURE_HELPER): tools/finer_visual_capture.m | $(BUILD_DIR)
	xcrun clang -fobjc-arc -O2 -Wall -Wextra -Werror \
		-framework AppKit -framework ApplicationServices \
		$< -o $@

check: build check-rules
	jq empty rules/generated/finder-vim.json
	./scripts/test_generated_rule.sh
	./scripts/test_mark_state.sh
	./scripts/test_tap_burst_headless.sh
	./scripts/test_column_phase_summary.sh
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

test-install: build
	./scripts/test_installation.sh

dist:
	./scripts/build_distribution.sh "$(VERSION)"

test-dist:
	./scripts/test_distribution.sh

benchmark-comparison-helpers:
	./scripts/build_comparison_helper.sh "$(BASELINE_REF)" baseline
	./scripts/build_comparison_helper.sh "$(CANDIDATE_REF)" candidate

benchmark-fixtures:
	FINDER_VIM_BENCHMARK_COUNTS="$(COUNTS)" \
		./scripts/prepare_benchmark_fixtures.sh

benchmark-realistic-fixtures:
	FINDER_VIM_FIXTURE_PROFILE=realistic-mixed \
	FINDER_VIM_BENCHMARK_COUNTS="$(COUNTS)" \
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

benchmark-column-realistic: benchmark-realistic-fixtures
	FINDER_VIM_FIXTURE_ROOT="$(CURDIR)/$(BUILD_DIR)/benchmark-fixtures/realistic-mixed" \
	FINDER_VIM_BENCHMARK_PROFILE=realistic-mixed \
	FINDER_VIM_BENCHMARK_COUNTS="$(COUNTS)" \
		./scripts/benchmark_column_jlj.sh "$(ITERATIONS)"

benchmark-list-realistic: benchmark-realistic-fixtures
	FINDER_VIM_FIXTURE_ROOT="$(CURDIR)/$(BUILD_DIR)/benchmark-fixtures/realistic-mixed" \
	FINDER_VIM_BENCHMARK_PROFILE=realistic-mixed \
	FINDER_VIM_BENCHMARK_COUNTS="$(COUNTS)" \
		./scripts/benchmark_view_navigation.sh list "$(ITERATIONS)"

benchmark-icon-realistic: benchmark-realistic-fixtures
	FINDER_VIM_FIXTURE_ROOT="$(CURDIR)/$(BUILD_DIR)/benchmark-fixtures/realistic-mixed" \
	FINDER_VIM_BENCHMARK_PROFILE=realistic-mixed \
	FINDER_VIM_BENCHMARK_COUNTS="$(COUNTS)" \
		./scripts/benchmark_view_navigation.sh icon "$(ITERATIONS)"

benchmark-realistic-views: benchmark-list-realistic benchmark-column-realistic benchmark-icon-realistic

benchmark-worker-timeout:
	FINDER_VIM_BENCHMARK_COUNTS=10 \
		./scripts/prepare_benchmark_fixtures.sh
	./scripts/benchmark_worker_idle_timeout.sh "$(ITERATIONS)"

benchmark-hold:
	FINDER_VIM_BENCHMARK_COUNTS=1000 \
		./scripts/prepare_benchmark_fixtures.sh
	./scripts/benchmark_hold_navigation.sh "$(ITERATIONS)"

benchmark-hold-realistic: benchmark-realistic-fixtures
	FINDER_VIM_FIXTURE_ROOT="$(CURDIR)/$(BUILD_DIR)/benchmark-fixtures/realistic-mixed" \
	FINDER_VIM_BENCHMARK_PROFILE=realistic-mixed \
		./scripts/benchmark_hold_navigation.sh "$(ITERATIONS)"

benchmark-hold-preflight:
	FINDER_VIM_BENCHMARK_COUNTS=1000 \
		./scripts/prepare_benchmark_fixtures.sh
	FINDER_VIM_BENCHMARK_PREFLIGHT=1 \
		./scripts/benchmark_hold_navigation.sh "$(ITERATIONS)"

benchmark-hold-realistic-preflight: benchmark-realistic-fixtures
	FINDER_VIM_FIXTURE_ROOT="$(CURDIR)/$(BUILD_DIR)/benchmark-fixtures/realistic-mixed" \
	FINDER_VIM_BENCHMARK_PROFILE=realistic-mixed \
	FINDER_VIM_BENCHMARK_PREFLIGHT=1 \
		./scripts/benchmark_hold_navigation.sh "$(ITERATIONS)"

benchmark-taps:
	FINDER_VIM_BENCHMARK_COUNTS=1000 \
		./scripts/prepare_benchmark_fixtures.sh
	./scripts/benchmark_tap_burst.sh "$(ITERATIONS)"

benchmark-taps-realistic: benchmark-realistic-fixtures
	FINDER_VIM_FIXTURE_ROOT="$(CURDIR)/$(BUILD_DIR)/benchmark-fixtures/realistic-mixed" \
	FINDER_VIM_BENCHMARK_PROFILE=realistic-mixed \
		./scripts/benchmark_tap_burst.sh "$(ITERATIONS)"

benchmark-taps-preflight:
	FINDER_VIM_BENCHMARK_COUNTS=1000 \
		./scripts/prepare_benchmark_fixtures.sh
	FINDER_VIM_BENCHMARK_PREFLIGHT=1 \
		./scripts/benchmark_tap_burst.sh "$(ITERATIONS)"

benchmark-taps-realistic-preflight: benchmark-realistic-fixtures
	FINDER_VIM_FIXTURE_ROOT="$(CURDIR)/$(BUILD_DIR)/benchmark-fixtures/realistic-mixed" \
	FINDER_VIM_BENCHMARK_PROFILE=realistic-mixed \
	FINDER_VIM_BENCHMARK_PREFLIGHT=1 \
		./scripts/benchmark_tap_burst.sh "$(ITERATIONS)"

benchmark-visual-helper: $(VISUAL_CAPTURE_HELPER)

benchmark-column-visual: benchmark-fixtures benchmark-visual-helper
	FINDER_VIM_BENCHMARK_COUNTS="$(COUNTS)" \
		./scripts/benchmark_column_visual_latency.sh "$(ITERATIONS)"

benchmark-column-visual-realistic: benchmark-realistic-fixtures benchmark-visual-helper
	FINDER_VIM_FIXTURE_ROOT="$(CURDIR)/$(BUILD_DIR)/benchmark-fixtures/realistic-mixed" \
	FINDER_VIM_BENCHMARK_PROFILE=realistic-mixed \
	FINDER_VIM_BENCHMARK_COUNTS="$(COUNTS)" \
		./scripts/benchmark_column_visual_latency.sh "$(ITERATIONS)"

test-visual-latency-analyzer:
	./scripts/test_visual_latency_analyzer.sh

test-finder-navigation: benchmark-fixtures
	./scripts/test_finder_navigation.sh

test-finder-selection: build
	./scripts/test_finder_selection.sh

clean:
	rm -rf $(BUILD_DIR)
