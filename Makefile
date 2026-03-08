.DEFAULT_GOAL := all

BUILD_DIR ?= build
CMAKE ?= cmake
LEGACY_ROOT_ARTIFACTS := \
	ipc-bench \
	libnetipc.a \
	netipc-codec-c \
	netipc-shm-server-demo \
	netipc-shm-client-demo \
	netipc-uds-server-demo \
	netipc-uds-client-demo
FORWARD_TARGETS := $(filter-out all configure clean,$(MAKECMDGOALS))

.PHONY: all configure clean $(FORWARD_TARGETS)

all: configure
	$(CMAKE) --build $(BUILD_DIR)

configure:
	$(CMAKE) -S . -B $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(LEGACY_ROOT_ARTIFACTS)

ifneq ($(FORWARD_TARGETS),)
$(FORWARD_TARGETS): configure
	$(CMAKE) --build $(BUILD_DIR) --target $@
endif
