#!/usr/bin/make -f
# Twirl as a DPF plugin: LV2, CLAP, VST3 and a JACK standalone.

NAME = twirl

FILES_DSP = TwirlPlugin.cpp
FILES_UI  = TwirlUI.cpp

CLAP_BUNDLE = true
UI_TYPE = webview
include ../../dpf/Makefile.plugins.mk

BUILD_CXX_FLAGS += -Wno-unused-parameter

TARGETS += jack
TARGETS += lv2
TARGETS += vst3
TARGETS += clap

# Twirl's page, one file, in each bundle's resources (the top Makefile fetches it).
PAGE = ../../build/index.html

clapfiles += $(TARGET_DIR)/$(NAME).clap/resources/index.html
jackfiles += $(TARGET_DIR)/resources/index.html
lv2files  += $(TARGET_DIR)/$(NAME).lv2/resources/index.html
vst3files += $(TARGET_DIR)/$(NAME).vst3/Contents/Resources/index.html

all: $(TARGETS) $(jackfiles) $(clapfiles) $(lv2files) $(vst3files)

%/index.html: $(PAGE)
	-@mkdir -p "$(dir $(abspath $@))"
	install -m 644 $< $(abspath $@)
