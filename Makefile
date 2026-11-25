# SPDX-License-Identifier: GPL-3.0-or-later
# Xbox 360 PC Wireless Gaming Receiver PS5 Payload Makefile

PS5_HOST ?=
PORT     ?= 9021

# Use PS5_PAYLOAD_SDK from environment if set; otherwise fall back to the
# ps5-payload-dev/sdk cached inside the user's ps5-native-app-boilerplate.
DEFAULT_SDK := /home/geert/Documents/_projects/ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk
PS5_PAYLOAD_SDK ?= $(DEFAULT_SDK)

ifeq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
$(error PS5_PAYLOAD_SDK ($(PS5_PAYLOAD_SDK)) does not contain toolchain/prospero.mk. Set PS5_PAYLOAD_SDK to your ps5-payload-dev/sdk installation.)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

BUILD_DIR     := build
TARGET        := $(BUILD_DIR)/Xbox360Receiver.elf
STATUS_TARGET := $(BUILD_DIR)/Xbox360Receiver-status.elf
STOP_TARGET   := $(BUILD_DIR)/Xbox360Receiver-stop.elf
GUITAR_TARGET := $(BUILD_DIR)/Xbox360Receiver-guitar.elf

CFLAGS += -D__PROSPERO__ -DX360W_EXTERNAL_SOURCE=1 -DPOORDS4_DATA_DIR=\"/data/x360w\" \
          -Wall -Wextra -Wshadow -Werror \
          -g -O2 -fPIC -fno-stack-protector -fno-jump-tables \
          -ffunction-sections -fdata-sections

LDFLAGS      += -Wl,--gc-sections,--build-id=sha1
RUNTIME_LIBS := -nodefaultlibs -lc -lkernel_web -lSceLibcInternal
BRIDGE_LIBS  := $(RUNTIME_LIBS) -lSceUserService

HOST_CC      ?= gcc
HOST_CFLAGS  := -std=c11 -O2 -Wall -Wextra -Werror -Ipayload

COMMON_HEADERS := $(wildcard payload/*.h)

.PHONY: all release status stop guitar test deploy clean help

all: $(TARGET) $(STATUS_TARGET) $(STOP_TARGET) $(GUITAR_TARGET)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(TARGET): payload/x360w_bridge_main.c payload/x360w_usb.c payload/x360w_protocol.c payload/wireless_ds4.c $(COMMON_HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ \
		payload/x360w_bridge_main.c \
		payload/x360w_usb.c \
		payload/x360w_protocol.c \
		payload/wireless_ds4.c \
		$(LDFLAGS) $(BRIDGE_LIBS)

status: $(STATUS_TARGET)

$(STATUS_TARGET): payload/x360w_status_main.c $(COMMON_HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ payload/x360w_status_main.c $(LDFLAGS) $(RUNTIME_LIBS)

stop: $(STOP_TARGET)

$(STOP_TARGET): payload/x360w_stop_main.c $(COMMON_HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ payload/x360w_stop_main.c $(LDFLAGS) $(RUNTIME_LIBS)

guitar: $(GUITAR_TARGET)

$(GUITAR_TARGET): payload/x360w_guitar_toggle_main.c $(COMMON_HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ payload/x360w_guitar_toggle_main.c $(LDFLAGS) $(RUNTIME_LIBS)

release: all
	$(STRIP) --strip-debug $(TARGET) $(STATUS_TARGET) $(STOP_TARGET) $(GUITAR_TARGET)

test: | $(BUILD_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -o $(BUILD_DIR)/test_x360w_protocol \
		tests/test_x360w_protocol.c payload/x360w_protocol.c
	./$(BUILD_DIR)/test_x360w_protocol

deploy: $(TARGET)
ifndef PS5_HOST
	$(error Set PS5_HOST=<ps5-ip> to deploy via elfldr (port $(PORT)))
endif
	nc -w 8 $(PS5_HOST) $(PORT) < $(TARGET)

clean:
	rm -rf $(BUILD_DIR)

help:
	@printf '%s\n' \
	  'make                         Build Xbox360Receiver.elf, status, and stop ELFs' \
	  'make release                 Build and strip debug symbols from all ELFs' \
	  'make test                    Build and run host-native protocol unit tests' \
	  'make deploy PS5_HOST=<ip>    Send Xbox360Receiver.elf to PS5 elfldr (port 9021)' \
	  'make clean                   Remove build outputs'
