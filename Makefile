.DEFAULT_GOAL := all

BUILD_DIR ?= build
CMAKE ?= cmake
GENERATED_ARTIFACTS := \
	src/crates/target
FORWARD_TARGETS := $(filter-out all configure clean,$(MAKECMDGOALS))

.PHONY: all configure clean $(FORWARD_TARGETS)

all: configure
	$(CMAKE) --build $(BUILD_DIR)

configure:
	$(CMAKE) -S . -B $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(GENERATED_ARTIFACTS)

ifneq ($(FORWARD_TARGETS),)
$(FORWARD_TARGETS): configure
	$(CMAKE) --build $(BUILD_DIR) --target $@
endif
