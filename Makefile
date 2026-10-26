#!/usr/bin/make -f
# Twirl as a DPF plugin. `make` builds everything into bin/; `make install`
# copies the LV2, CLAP and VST3 into ~/.lv2, ~/.clap and ~/.vst3.
#
# Twirl's page is copied, not forked: from a music-suite checkout beside this
# one (MUSIC_SUITE=…/music-suite to point elsewhere), else from GitHub's main.

include dpf/Makefile.base.mk

MUSIC_SUITE ?= ../music-suite
TWIRL_URL   ?= https://raw.githubusercontent.com/Enkerli/music-suite/main/apps/twirl/index.html

all: patch dgl page plugins gen

# DPF develop, with the fixes in patches/ (until they're upstream).
patch:
	@for p in patches/*.patch; do \
	  if git -C dpf apply --reverse --check ../$$p 2>/dev/null; then :; \
	  else echo "applying $$p"; git -C dpf apply ../$$p; fi; \
	done

dgl:
	$(MAKE) -C dpf/dgl USE_WEB_VIEW=true

page:
	@mkdir -p build
	@if [ -f "$(MUSIC_SUITE)/apps/twirl/index.html" ]; then \
	  cmp -s "$(MUSIC_SUITE)/apps/twirl/index.html" build/index.html || cp "$(MUSIC_SUITE)/apps/twirl/index.html" build/index.html; \
	  echo "Twirl's page: $(MUSIC_SUITE)/apps/twirl/index.html"; \
	else \
	  curl -fsSL -o build/index.html.new "$(TWIRL_URL)" && mv build/index.html.new build/index.html; \
	  echo "Twirl's page: $(TWIRL_URL)"; \
	fi

plugins: dgl page
	$(MAKE) all -C plugins/Twirl

ifneq ($(CROSS_COMPILING),true)
gen: plugins dpf/utils/lv2_ttl_generator
	@$(CURDIR)/dpf/utils/generate-ttl.sh
dpf/utils/lv2_ttl_generator:
	$(MAKE) -C dpf/utils/lv2-ttl-generator
else
gen:
endif

install: all
	mkdir -p ~/.lv2 ~/.clap ~/.vst3
	rm -rf ~/.lv2/twirl.lv2 ~/.clap/twirl.clap ~/.vst3/twirl.vst3
	cp -r bin/twirl.lv2 ~/.lv2/
	cp -r bin/twirl.clap ~/.clap/
	cp -r bin/twirl.vst3 ~/.vst3/

clean:
	$(MAKE) clean -C dpf/dgl
	$(MAKE) clean -C plugins/Twirl
	rm -rf bin build

# Twirl's engine, offline (tests/render_test.cpp): each voice sounds, doesn't
# clip, dies away; WAVs in build/render/ for listening.
test:
	@mkdir -p build/render
	$(CXX) -std=c++17 -O2 -Wall -Wextra -o build/render_test tests/render_test.cpp
	./build/render_test

.PHONY: all patch dgl page plugins gen install clean test
