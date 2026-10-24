.SUFFIXES:
.DEFAULT_GOAL := all

TARGET := nxhid
BUILD := build
APP_TITLE := NXHID Wi-Fi Keyboard
APP_AUTHOR := NXHID
APP_VERSION := 0.2.0
PYTHON ?= python3

.PHONY: test clean
test:
	$(PYTHON) -B -m unittest discover -s tests -v
clean:
	rm -rf $(BUILD) dist $(TARGET).nro $(TARGET).elf $(TARGET).nacp

# Host tests and cleanup do not require the Switch SDK.
ifneq ($(filter-out test clean,$(or $(MAKECMDGOALS),all)),)
DEVKITPRO ?= /opt/devkitpro
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -O2 -Wall -Wextra -Werror -std=gnu11 -ffunction-sections $(ARCH) $(INCLUDE) -D__SWITCH__
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS := -lnx

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(CURDIR)/source
export DEPSDIR := $(CURDIR)/$(BUILD)
export OFILES := $(notdir $(patsubst %.c,%.o,$(wildcard source/*.c)))
export INCLUDE := -I$(CURDIR)/include -I$(LIBNX)/include
export LIBPATHS := -L$(LIBNX)/lib
export LD := $(CC)
export NROFLAGS := --icon=$(LIBNX)/default_icon.jpg --nacp=$(CURDIR)/$(TARGET).nacp
.PHONY: all $(BUILD) package
all: $(BUILD)
$(BUILD):
	@mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
package: all
	$(PYTHON) -B tools/package.py $(APP_VERSION)
else
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(OFILES:.o=.d)
endif
endif
