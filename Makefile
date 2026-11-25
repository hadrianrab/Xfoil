ifeq ($(strip $(DEVKITPRO)),)
$(error Configure DEVKITPRO and install switch-dev)
endif
include $(DEVKITPRO)/libnx/switch_rules

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CPPFLAGS := -I$(DEVKITPRO)/libnx/include -I$(PORTLIBS)/include -Ivendor/cjson -D__SWITCH__
CFLAGS := $(ARCH) -O2 -Wall -Wextra -ffunction-sections -fdata-sections
VERSION := $(shell sed -n 's/^\#define APP_VERSION "\(.*\)"/\1/p' source/main.c)

.PHONY: all
all: xfoil.nro

xfoil.elf: source/main.c vendor/cjson/cJSON.c source/hud.inc source/ui_assets.inc
	$(CC) $(CPPFLAGS) $(CFLAGS) -specs=$(DEVKITPRO)/libnx/switch.specs $(filter %.c,$^) -L$(PORTLIBS)/lib -L$(DEVKITPRO)/libnx/lib -lcurl -lz -lnx -lm -o $@

xfoil.nacp: source/main.c
	nacptool --create "Xfoil" "IndicaX" "$(VERSION)" $@

xfoil.nro: xfoil.elf xfoil.nacp icon.jpg
	elf2nro $< $@ --icon=icon.jpg --nacp=xfoil.nacp
