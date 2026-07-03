CXX ?= g++
CXXFLAGS ?= -O2 -pipe -std=c++20 -Wall -Wextra -Wpedantic
CPUFLAGS ?= -march=x86-64 -mtune=generic -mno-avx -mno-avx2

BIN_DIR := bin

# CORE APPLICATIONS
DEVTOOL := $(BIN_DIR)/dev
DADA := $(BIN_DIR)/dada
CITRON := $(BIN_DIR)/citron
INFINITAS := $(BIN_DIR)/infinitas
LEV_DASH := $(BIN_DIR)/leviathan-dashboard

# OFFICE SUITE (Precompiled - already in bin/)
SCRIBE := $(BIN_DIR)/scribe
TABULA := $(BIN_DIR)/tabula
SWIPE := $(BIN_DIR)/swipe
APPBASE := $(BIN_DIR)/appbase

# UTILITIES
LEV_CONFIG := $(BIN_DIR)/leviathan
OLAUNCH := $(BIN_DIR)/olaunch
ARCHITECT := $(BIN_DIR)/architect

# TESTING (Optional)
DEVVM_TEST := $(BIN_DIR)/test_devvm

# Prevent implicit rules
.SUFFIXES:

# Prefer webkit2gtk-4.1 or fall back to 4.0
WEBKIT_PKG := $(or \
  $(shell pkg-config --exists webkit2gtk-4.1 2>/dev/null && echo webkit2gtk-4.1), \
  $(shell pkg-config --exists webkit2gtk-4.0 2>/dev/null && echo webkit2gtk-4.0))
DADA_PKG := $(shell pkg-config --cflags --libs gtk+-3.0 $(WEBKIT_PKG) 2>/dev/null)
LEV_PKG := $(DADA_PKG)

.PHONY: all clean devvm-test office verify churro

# Default target: build core + office suite + churro desktop
all: $(DEVTOOL) $(DADA) $(CITRON) $(INFINITAS) $(LEV_DASH) $(LEV_CONFIG) $(OLAUNCH) $(SCRIBE) $(TABULA) $(SWIPE) $(APPBASE) $(ARCHITECT) churro

office: $(SCRIBE) $(TABULA) $(SWIPE) $(APPBASE)

# ════════════════════════════════════════════════════════════
# BUILD RULES - Compile from source
# ════════════════════════════════════════════════════════════

# devtool links the DevVM JIT→C / AOT→C compiler so `dev run --produce-binary`
# is a real ahead-of-time bytecode→C→gcc pipeline (not a stub).
$(DEVTOOL): src/devtool/main.cpp src/devvm/jit/jit_to_c.cpp src/devvm/ir/instructions.cpp src/devvm/jit/jit_to_c.h
	mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(CPUFLAGS) -Isrc/devvm/jit src/devtool/main.cpp src/devvm/jit/jit_to_c.cpp src/devvm/ir/instructions.cpp -o $@ -lzstd

$(DADA): src/dada/main.cpp
	mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(CPUFLAGS) -Ithird_party/webview $< -o $@ $(DADA_PKG)

$(CITRON): $(wildcard src/citron/*.c src/citron/*.h) src/citron/Makefile
	mkdir -p $(BIN_DIR)
	$(MAKE) -C src/citron
	cp src/citron/citron $(BIN_DIR)/citron
	chmod 0755 $(BIN_DIR)/citron

# NOTE: these rules list their sources as prerequisites so `make` actually
# rebuilds bin/<app> when the source changes. Without prerequisites make treats
# an existing bin/<app> as up-to-date and silently ships a stale binary.
$(SCRIBE): src/scribe/scribe.c src/scribe/Makefile
	mkdir -p $(BIN_DIR)
	$(MAKE) -C src/scribe clean && $(MAKE) -C src/scribe
	cp src/scribe/scribe $(BIN_DIR)/scribe
	chmod 0755 $(BIN_DIR)/scribe

$(TABULA): src/tabula/tabula.c src/tabula/Makefile
	mkdir -p $(BIN_DIR)
	$(MAKE) -C src/tabula clean && $(MAKE) -C src/tabula
	cp src/tabula/tabula $(BIN_DIR)/tabula
	chmod 0755 $(BIN_DIR)/tabula

$(SWIPE): src/swipe/swipe.c src/swipe/pptx.c src/swipe/Makefile
	mkdir -p $(BIN_DIR)
	$(MAKE) -C src/swipe clean && $(MAKE) -C src/swipe
	cp src/swipe/swipe $(BIN_DIR)/swipe
	chmod 0755 $(BIN_DIR)/swipe

$(APPBASE): src/appbase/appbase.c src/appbase/datac.c src/appbase/datac.h src/appbase/Makefile
	mkdir -p $(BIN_DIR)
	$(MAKE) -C src/appbase clean && $(MAKE) -C src/appbase
	cp src/appbase/appbase $(BIN_DIR)/appbase
	chmod 0755 $(BIN_DIR)/appbase

$(ARCHITECT): src/architect/architect.cpp src/architect/lsp.cpp src/architect/lsp.hpp src/architect/json.hpp src/architect/Makefile
	mkdir -p $(BIN_DIR)
	$(MAKE) -C src/architect clean && $(MAKE) -C src/architect
	cp src/architect/architect $(BIN_DIR)/architect
	chmod 0755 $(BIN_DIR)/architect

$(LEV_DASH): src/legacy_dashboard/main.cpp
	mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(CPUFLAGS) -Ithird_party/webview $< -o $@ $(LEV_PKG)

$(INFINITAS): $(wildcard src/infinitas/src/*.c src/infinitas/src/*.h) src/infinitas/Makefile
	$(MAKE) -C src/infinitas
	mkdir -p $(BIN_DIR)
	cp src/infinitas/infinitas $(BIN_DIR)/infinitas

$(LEV_CONFIG): scripts/leviathan-config.sh
	mkdir -p $(BIN_DIR)
	cp scripts/leviathan-config.sh $(BIN_DIR)/leviathan
	chmod 0755 $(BIN_DIR)/leviathan

$(OLAUNCH): $(wildcard src/olaunch/olaunch.c src/olaunch/*.h) src/olaunch/Makefile
	mkdir -p $(BIN_DIR)
	$(MAKE) -C src/olaunch
	cp src/olaunch/olaunch $(BIN_DIR)/olaunch
	chmod 0755 $(BIN_DIR)/olaunch

churro:
	$(MAKE) -C src/churro

# ════════════════════════════════════════════════════════════
# TEST TARGET (Optional)
# ════════════════════════════════════════════════════════════

$(DEVVM_TEST): src/devvm/tests/test_devvm.cpp src/devvm/core/devvm.cpp src/devvm/ir/instructions.cpp src/devvm/jit/x86_64.cpp src/devvm/jit/i686.cpp src/devvm/security/sandbox.cpp
	mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(CPUFLAGS) $^ -o $@

devvm-test: $(DEVVM_TEST)
	$(DEVVM_TEST)

clean:
	rm -rf $(BIN_DIR)
