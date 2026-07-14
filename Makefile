SHELL := /bin/zsh

BUILD_DIR := .build
MODULE_CACHE := $(BUILD_DIR)/swift-module-cache
C_HELPER := $(BUILD_DIR)/finder_ax_step
SWIFT_HELPER := $(BUILD_DIR)/finder_ax_move

.PHONY: all build check clean install uninstall

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
	zsh -n scripts/finder_action_marked.sh scripts/finder_paste.sh
	! rg -n '/Users/[^/]+/' . \
		--glob '!docs/FINDER_VIM_SPEC.md' \
		--glob '!README.md' \
		--glob '!Makefile' \
		--glob '!.build/**'

install: build
	./scripts/install.sh

uninstall:
	./scripts/uninstall.sh

clean:
	rm -rf $(BUILD_DIR)
