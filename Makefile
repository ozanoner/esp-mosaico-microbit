# Everything runs in containers. Only Docker is required on the host.
#
#   make verify      reproducibility check (tests + rebuild + golden compare)
#   make build       build a payload to build/<name>.hex
#   make up          start the brain service on :8080
#   make logs        follow the brain log
#   make firmware    shell inside the ESP-IDF container (mosaico.py lives there)
#   make test        unit tests only
#   make down        stop everything

SHELL := /bin/bash
COMPOSE := docker compose
BRAIN := $(COMPOSE) run --rm brain
PAYLOAD ?= microbit/payloads/hello.ts

.DEFAULT_GOAL := help
.PHONY: help verify test build up logs down firmware ps clean

help:
	@grep -E '^#   make' Makefile | sed 's/^#   //'

verify:
	$(BRAIN) verify

test:
	$(BRAIN) test

build:
	@mkdir -p build
	$(BRAIN) build $(PAYLOAD) -o build/$(notdir $(basename $(PAYLOAD))).hex

up:
	$(COMPOSE) up -d --build brain
	@echo "brain: http://localhost:8080/v1/health"

logs:
	$(COMPOSE) logs -f brain

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
		rm -rf build brain/.work brain/esp_mb_brain.egg-info tools/hexkit/hexkit.egg-info; \
		find . -name __pycache__ -type d -prune -exec rm -rf {} + ; \
		find . -name .pytest_cache -type d -prune -exec rm -rf {} + '
