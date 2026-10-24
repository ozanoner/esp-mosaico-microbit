# Everything runs in containers. Only Docker is required on the host.
#
#   make verify      reproducibility check (tests + rebuild + golden compare)
#   make build       build a payload to build/<name>.hex
#   make up          start the build service on :8477
#   make logs        follow the build service log
#   make firmware    shell inside the ESP-IDF container (mosaico.py lives there)
#   make test        unit tests only
#   make down        stop everything

SHELL := /bin/bash
COMPOSE := docker compose
BUILD_SERVICE := $(COMPOSE) run --rm build-service
PAYLOAD ?= microbit/payloads/hello.ts

.DEFAULT_GOAL := help
.PHONY: help verify test build up logs down firmware ps clean

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

firmware:
	$(COMPOSE) up -d esp-idf
	$(COMPOSE) exec esp-idf bash -lc 'cd /workspace/firmware && exec bash'

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
