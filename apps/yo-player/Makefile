.SUFFIXES:
.DEFAULT_GOAL := all

ifeq ($(strip $(PSL1GHT)),)
$(error "Set PSL1GHT to the installed PSL1GHT directory")
endif

include $(PSL1GHT)/ppu_rules

# GNU make supplies a built-in LD=ld, so PSL1GHT's conditional LD assignment
# does not replace it.  Link through the PPU GCC driver selected by ppu_rules.
LD := $(CC)

TARGET  := yo-player-psl1ght
PKG_NAME := crtube
BUILD   := build/psl1ght
SOURCES := src src/screens src/extractors psl1ght
INCLUDES := include ../../libs/simple-lib-app/include ../../libs/simple-lib-av/include \
            ../../libs/simple-lib-https/include ../../libs/simple-lib-core/include psl1ght/include

CFILES := main.c home.c play.c search.c playlist-picker.c video-grid.c storage.c stream-select.c \
          downloads.c video-export-psl1ght.c extractor.c youtube.c sponsorblock.c \
          subtitles.c chapters.c settings.c updater.c theme.c screensaver.c \
          account.c account-oauth.c youtube-tv.c sidebar.c

PROJECT_DIR := $(patsubst %/,%,$(dir $(abspath $(firstword $(MAKEFILE_LIST)))))

TITLE     := CRTube
APPID     := CRTUBE001
CONTENTID := UP0001-$(APPID)_00-0000000000000000
ICON0            := $(PROJECT_DIR)/ICON0.PNG
VERSION_FILE     := $(PROJECT_DIR)/VERSION
VERSION_HEADER   := $(PROJECT_DIR)/include/version.h
SFOXML           := $(PROJECT_DIR)/$(BUILD)/PARAM.SFO.xml
PKGFILES         := $(CURDIR)/pkgfiles
SCREENSAVER_LOGO := $(CURDIR)/res/BOUNCING_LOGO.JPG

ROOT        := $(abspath $(PROJECT_DIR)/../..)
CORE_LIB   := $(ROOT)/libs/simple-lib-core/build/psl1ght/libsimple-lib-core.a
HTTPS_LIB  := $(ROOT)/libs/simple-lib-https/build/psl1ght/libsimple-lib-https.a
AV_LIB     := $(ROOT)/libs/simple-lib-av/build/psl1ght/libsimple-lib-av.a
APP_LIB    := $(ROOT)/libs/simple-lib-app/build/psl1ght/libsimple-lib-app.a
PORTLIBS   := $(PS3DEV)/portlibs/ppu

$(VERSION_HEADER): $(VERSION_FILE) $(PROJECT_DIR)/psl1ght/generate_version_header.py
	@python3 $(PROJECT_DIR)/psl1ght/generate_version_header.py $(VERSION_FILE) $@

$(SFOXML): $(PROJECT_DIR)/psl1ght/PARAM.SFO.xml $(VERSION_FILE)
	@python3 $(PROJECT_DIR)/psl1ght/generate_sfo.py $< $(VERSION_FILE) $@

CFLAGS = -O2 -Wall -Wextra -std=gnu11 -mcpu=cell $(MACHDEP) $(INCLUDE) \
          -DPSL1GHT -DNDEBUG -include psl1ght_compat.h \
          -DBUILD_NAME=\"CRTube\" -DBUILD_STAMP=\"$(shell git -C $(ROOT) rev-parse --short HEAD 2>/dev/null || echo unknown)\"
ASFLAGS = $(MACHDEP) -mregnames -mcpu=cell -Wa,-mcell $(INCLUDE)
# The compiler driver links PSL1GHT's lv2-sprx.o as a start file, and its 0x28-byte
# process PRX parameters would sit in front of lv2-sprx-modern.o's.  -B finds this
# app's empty lv2-sprx.o first, so the installed toolchain stays untouched.
STARTFILES := startfiles
LDFLAGS = $(MACHDEP) -B$(BUILDDIR)/$(STARTFILES)/ -Wl,--gc-sections -Wl,-Map,$(notdir $@).map
PROJECT_LIBDIRS := $(ROOT)/libs/simple-lib-app/build/psl1ght \
                   $(ROOT)/libs/simple-lib-av/build/psl1ght \
                   $(ROOT)/libs/simple-lib-https/build/psl1ght \
                   $(ROOT)/libs/simple-lib-core/build/psl1ght
LIBS := -lsimple-lib-app -lsimple-lib-av -lsimple-lib-https -lsimple-lib-core \
        -lfaad -lvdec -laudio -lrsx -lgcm_sys -lio -lsysutil \
        -lpngdec -ljpeg -lsysmodule -lnet -lsysfs -lrt -llv2 -lm

ifneq ($(notdir $(BUILD)),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
export BUILDDIR := $(CURDIR)/$(BUILD)
# sprxlinker derives each import count from the distance between consecutive
# FNID marker symbols.  Keep every assembly marker immediately before its C
# table object; grouping all .S files separately would make both counts zero.
export OFILES := lv2-sprx-modern.o $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  -I$(PORTLIBS)/include $(LIBPSL1GHT_INC) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(PROJECT_LIBDIRS),-L$(dir)) -L$(PORTLIBS)/lib $(LIBPSL1GHT_LIB)

.PHONY: all libraries $(CORE_LIB) $(HTTPS_LIB) $(AV_LIB) $(APP_LIB) $(BUILD) clean run pkg
all: $(BUILD)

libraries: $(CORE_LIB) $(HTTPS_LIB) $(AV_LIB) $(APP_LIB)

$(CORE_LIB):
	@$(MAKE) --no-print-directory -C $(ROOT)/libs/simple-lib-core -f Makefile.psl1ght

$(HTTPS_LIB):
	@$(MAKE) --no-print-directory -C $(ROOT)/libs/simple-lib-https -f Makefile.psl1ght

$(AV_LIB):
	@$(MAKE) --no-print-directory -C $(ROOT)/libs/simple-lib-av -f Makefile.psl1ght

$(APP_LIB):
	@$(MAKE) --no-print-directory -C $(ROOT)/libs/simple-lib-app -f Makefile.psl1ght

$(BUILD): libraries $(VERSION_HEADER)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $@ -f $(CURDIR)/Makefile $(OUTPUT).elf

clean:
	@$(MAKE) --no-print-directory -C $(ROOT)/libs/simple-lib-core -f Makefile.psl1ght clean
	@$(MAKE) --no-print-directory -C $(ROOT)/libs/simple-lib-https -f Makefile.psl1ght clean
	@$(MAKE) --no-print-directory -C $(ROOT)/libs/simple-lib-av -f Makefile.psl1ght clean
	@$(MAKE) --no-print-directory -C $(ROOT)/libs/simple-lib-app -f Makefile.psl1ght clean
	@rm -rf $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).fake.self $(OUTPUT).pkg $(OUTPUT).gnpdrm.pkg $(CURDIR)/$(PKG_NAME).pkg $(CURDIR)/$(PKG_NAME).gnpdrm.pkg

run: $(BUILD)
	@$(PS3LOADAPP) $(OUTPUT).self

# Keep package creation behind the completed recursive build even under -j.
# The conventional `pkg: $(BUILD) $(OUTPUT).pkg` form permits both prerequisites
# to run concurrently and can start a second object build in this directory.
pkg: $(BUILD)
	@test -f $(SCREENSAVER_LOGO) || { echo "Missing screensaver logo: $(SCREENSAVER_LOGO)"; exit 1; }
	@mkdir -p $(PKGFILES)/USRDIR
	@cp $(SCREENSAVER_LOGO) $(PKGFILES)/USRDIR/BOUNCING_LOGO.JPG
	@rm -f $(OUTPUT).self $(OUTPUT).fake.self $(OUTPUT).pkg $(OUTPUT).gnpdrm.pkg $(CURDIR)/$(PKG_NAME).pkg $(CURDIR)/$(PKG_NAME).gnpdrm.pkg
	# Build SELF from the recursive build directory, where this project overrides
	# PSL1GHT's generic %.self rule with the required PRX metadata processing.
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(PROJECT_DIR)/Makefile $(OUTPUT).self
	@$(MAKE) --no-print-directory $(OUTPUT).pkg
	@cp $(OUTPUT).pkg $(CURDIR)/$(PKG_NAME).pkg
	@if [ -f $(OUTPUT).gnpdrm.pkg ]; then cp $(OUTPUT).gnpdrm.pkg $(CURDIR)/$(PKG_NAME).gnpdrm.pkg; fi
else
DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
	@echo CEX self ... $(notdir $@)
	@mkdir -p $(BUILDDIR)
	@cp $< $(BUILDDIR)/$(notdir $<)
	@python3 $(PROJECT_DIR)/psl1ght/set_prx_sdk_version.py \
		$(BUILDDIR)/$(notdir $<) --sprxlinker-compat
	@$(SPRX) $(BUILDDIR)/$(notdir $<)
	@python3 $(PROJECT_DIR)/psl1ght/set_prx_sdk_version.py \
		$(BUILDDIR)/$(notdir $<) --sdk 0x00475001
	@$(SELF) $(BUILDDIR)/$(notdir $<) $@
	@$(FSELF) $(BUILDDIR)/$(notdir $<) $(basename $@).fake.self
$(STARTFILES)/lv2-sprx.o: lv2-sprx-empty.S
	@mkdir -p $(STARTFILES)
	@$(CC) -c $(ASFLAGS) $< -o $@

$(OUTPUT).elf: $(OFILES) $(APP_LIB) $(AV_LIB) $(HTTPS_LIB) $(CORE_LIB) | $(STARTFILES)/lv2-sprx.o
-include $(DEPENDS)
endif

$(OUTPUT).pkg: $(SFOXML)
$(OUTPUT).gnpdrm.pkg: $(SFOXML)
