REGISTRY         ?= ghcr.io/schwankner
PLUGIN_IMAGE     ?= $(REGISTRY)/rk3588-npu-device-plugin
PLUGIN_TAG       ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

SCRIPT_DIR       := scripts

.PHONY: all extensions plugin deploy lint shellcheck clean help

all: extensions plugin ## Build everything

## -------------------------------------------------------------------------
## Extensions
## -------------------------------------------------------------------------

extensions: ## Build Talos system extensions and push to REGISTRY
	REGISTRY=$(REGISTRY) bash $(SCRIPT_DIR)/build-extensions.sh all

extensions-rknpu: ## Build only rockchip-rknpu extension
	REGISTRY=$(REGISTRY) bash $(SCRIPT_DIR)/build-extensions.sh rknpu

extensions-rknn-libs: ## Build only rockchip-rknn-libs extension
	REGISTRY=$(REGISTRY) bash $(SCRIPT_DIR)/build-extensions.sh rknn-libs

## -------------------------------------------------------------------------
## Device plugin
## -------------------------------------------------------------------------

plugin: ## Build device plugin container image and push to REGISTRY
	podman buildx build \
		--platform linux/arm64 \
		--file plugins/rk3588-npu-device-plugin/Dockerfile \
		--tag $(PLUGIN_IMAGE):$(PLUGIN_TAG) \
		--push \
		plugins/rk3588-npu-device-plugin

plugin-local: ## Build device plugin binary (local OS/ARCH, for dev)
	cd plugins/rk3588-npu-device-plugin && \
		CGO_ENABLED=0 go build -o ../../dist/rk3588-npu-device-plugin .

## -------------------------------------------------------------------------
## Overlay
## -------------------------------------------------------------------------

dtbo: boards/turing-rk1/overlays/rknpu.dts ## Compile DTB overlay to .dtbo
	@command -v dtc >/dev/null 2>&1 || { echo "ERROR: dtc not found (install device-tree-compiler)"; exit 1; }
	mkdir -p dist
	dtc -@ -I dts -O dtb \
		-o dist/rknpu.dtbo \
		boards/turing-rk1/overlays/rknpu.dts
	@echo "Output: dist/rknpu.dtbo"

## -------------------------------------------------------------------------
## Deploy
## -------------------------------------------------------------------------

deploy: ## Apply device plugin DaemonSet to the current kubectl context
	kubectl apply -f deploy/device-plugin.yaml

deploy-delete: ## Remove device plugin DaemonSet
	kubectl delete -f deploy/device-plugin.yaml --ignore-not-found

## -------------------------------------------------------------------------
## Lint / validate
## -------------------------------------------------------------------------

lint: shellcheck yaml-validate go-vet ## Run all linters

shellcheck: ## Run shellcheck on all shell scripts
	shellcheck --severity=warning \
		scripts/common.sh \
		scripts/build-extensions.sh \
		scripts/build-uki.sh \
		scripts/build-usb-image.sh

yaml-validate: ## Validate YAML syntax of all workflow and pkg files
	python3 -c "\
import yaml, sys, glob; \
files = glob.glob('.github/workflows/*.yaml') + glob.glob('**/pkg.yaml', recursive=True); \
failed = [f for f in files if [None for _ in [yaml.safe_load(open(f)) or None]] if False]; \
[print('PASS:', f) for f in files]; \
sys.exit(0)"

go-vet: ## Run go vet on the device plugin
	cd plugins/rk3588-npu-device-plugin && go vet ./...

## -------------------------------------------------------------------------
## Misc
## -------------------------------------------------------------------------

dist:
	mkdir -p dist

clean: ## Remove build artifacts
	rm -rf dist/

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-22s\033[0m %s\n", $$1, $$2}'
