# Everything runs in containers. Only Docker is required on the host.
#
#   make verify      reproducibility check (tests + rebuild + golden compare)
#   make build       build a payload to build/<name>.hex
#   make up          start the build service on :8477
#   make logs        follow the build service log
#   make firmware    shell inside the ESP-IDF container (mosaico.py lives there)
#   make firmware-hw same shell, with the board's USB ports exposed
#   make patch-tools apply the local fix to the pinned esp-mosaico-utils tools
#   make test        unit tests only
#   make down        stop everything

SHELL := /bin/bash
COMPOSE := docker compose
# Hardware overlay: only for a workstation with the board plugged in.
COMPOSE_HW := $(COMPOSE) -f compose.yaml -f compose.hw.yaml
BUILD_SERVICE := $(COMPOSE) run --rm build-service
PAYLOAD ?= microbit/payloads/hello.ts

# Local fix to the pinned vendor tools; see firmware/patches/README.md.
TOOLS_DIR := firmware/submodule/esp-mosaico-utils
TOOLS_PATCH := $(abspath firmware/patches/0001-rom-download-mode-discovery.patch)

.DEFAULT_GOAL := help
.PHONY: help verify test build up logs down firmware firmware-hw patch-tools ps clean

help:
	@grep -E '^#   make' Makefile | sed 's/^#   //'

verify:
	$(BUILD_SERVICE) verify

test:
	$(BUILD_SERVICE) test

build:
	@mkdir -p build
	$(BUILD_SERVICE) build $(PAYLOAD) -o build/$(notdir $(basename $(PAYLOAD))).hex

up:
	$(COMPOSE) up -d --build build-service
	@echo "build-service: http://localhost:$${BUILD_SERVICE_PORT:-8477}/v1/health"

logs:
	$(COMPOSE) logs -f build-service

patch-tools:
	@if git -C $(TOOLS_DIR) apply --reverse --check $(TOOLS_PATCH) 2>/dev/null; then \
		echo "tools patch: already applied"; \
	else \
		git -C $(TOOLS_DIR) apply $(TOOLS_PATCH) && echo "tools patch: applied"; \
	fi

firmware: patch-tools
	$(COMPOSE) up -d mosaico-idf
	$(COMPOSE) exec mosaico-idf bash -lc 'cd /workspace/firmware && exec bash'

firmware-hw: patch-tools
	$(COMPOSE_HW) up -d mosaico-idf
	$(COMPOSE_HW) exec mosaico-idf bash -lc 'cd /workspace/firmware && exec bash'

ps:
	$(COMPOSE) ps

down:
	$(COMPOSE) down

clean:
	# container-written files are root-owned; delete them from a container
	docker run --rm -v "$(CURDIR)":/w -w /w alpine:latest sh -c '\
		rm -rf build build-service/.work build-service/mb_build_service.egg-info tools/hexkit/hexkit.egg-info; \
		find . -name __pycache__ -type d -prune -exec rm -rf {} + ; \
		find . -name .pytest_cache -type d -prune -exec rm -rf {} + '
