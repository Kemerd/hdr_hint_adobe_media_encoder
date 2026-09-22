# ---------------------------------------------------------------------------
# Makefile - a thin, friendly front end for the CMake build.
#
# CMake does the real work; this file just remembers the flags. It runs on
# macOS (Apple clang + Ninja or Make) and on Windows (GNU make from Git for
# Windows / MSYS2 / Chocolatey, driving the Visual Studio generator).
#
#   make                 Release build of everything (app, CLI, tests)
#   make test            build, then run the unit tests
#   make debug           Debug build in build-debug/
#   make app | cli       just the GUI app / just the command-line tool
#   make run             build and start the app
#   make screenshots     render every tab, dark and light, to build/screenshots
#   make package         DMG on macOS, ZIP on Windows (CPack)
#   make install         install into PREFIX (default: ./dist)
#   make panel           install the Media Encoder panel for this user
#   make icon            regenerate resources/macos/HdrHint.icns
#   make clean           clean the build tree; `make distclean` deletes it
#
# Knobs (override on the command line, e.g. `make UNIVERSAL=ON package`):
#   CONFIG     Release | Debug | RelWithDebInfo          (default Release)
#   BUILD_DIR  build directory                           (default build)
#   UNIVERSAL  ON = arm64 + x86_64 on macOS              (default OFF)
#   GENERATOR  CMake generator                           (default: Ninja if installed)
#   PREFIX     install prefix for `make install`         (default ./dist)
#   JOBS       parallel jobs                             (default: all cores)
# ---------------------------------------------------------------------------

CONFIG    ?= Release
BUILD_DIR ?= build
UNIVERSAL ?= OFF
PREFIX    ?= $(CURDIR)/dist
CMAKE     ?= cmake
CTEST     ?= ctest
CPACK     ?= cpack
PYTHON    ?= python3

# ---- platform ---------------------------------------------------------------
ifeq ($(OS),Windows_NT)
    PLATFORM     := windows
    EXE          := .exe
    # Visual Studio is multi-config: binaries land in <build>/<config>/.
    BIN_DIR       = $(BUILD_DIR)/$(CONFIG)
    APP           = $(BIN_DIR)/HdrHint.exe
    APP_BINARY    = $(APP)
    GENERATOR    ?=
    PYTHON       := python
else
    UNAME := $(shell uname -s)
    ifeq ($(UNAME),Darwin)
        PLATFORM := macos
    else
        PLATFORM := posix
    endif
    EXE          :=
    BIN_DIR       = $(BUILD_DIR)
    APP           = $(BUILD_DIR)/HdrHint.app
    APP_BINARY    = $(APP)/Contents/MacOS/HdrHint
    # Ninja when available (fast, and CI uses it); Unix Makefiles otherwise.
    GENERATOR    ?= $(if $(shell command -v ninja 2>/dev/null),Ninja,)
endif

CLI := $(BIN_DIR)/hdrhint_cli$(EXE)

# ---- parallelism --------------------------------------------------------------
ifdef JOBS
    PARALLEL := --parallel $(JOBS)
else
    PARALLEL := --parallel
endif

# ---- configure flags -------------------------------------------------------------
CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=$(CONFIG)
ifneq ($(strip $(GENERATOR)),)
    CMAKE_FLAGS += -G "$(GENERATOR)"
endif
ifeq ($(PLATFORM),macos)
    CMAKE_FLAGS += -DHH_UNIVERSAL=$(UNIVERSAL)
endif
CMAKE_FLAGS += $(EXTRA_CMAKE_FLAGS)

# Screenshot output folder.
SHOTS := $(BUILD_DIR)/screenshots

.PHONY: all release debug configure build app cli tests test run screenshots \
        package dmg zip install panel unpanel icon clean distclean help

all: build

release:
	@$(MAKE) --no-print-directory CONFIG=Release build

debug:
	@$(MAKE) --no-print-directory CONFIG=Debug BUILD_DIR=build-debug build

help:
	@sed -n '2,31p' Makefile | sed 's/^# \{0,1\}//'

# ---- configure / build --------------------------------------------------------------
# Configure once; later runs reuse the cache (CMake re-runs itself when
# CMakeLists.txt changes).
$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_FLAGS)

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_FLAGS)

build: $(BUILD_DIR)/CMakeCache.txt
	$(CMAKE) --build $(BUILD_DIR) --config $(CONFIG) $(PARALLEL)

app: $(BUILD_DIR)/CMakeCache.txt
	$(CMAKE) --build $(BUILD_DIR) --config $(CONFIG) $(PARALLEL) --target HdrHint

cli: $(BUILD_DIR)/CMakeCache.txt
	$(CMAKE) --build $(BUILD_DIR) --config $(CONFIG) $(PARALLEL) --target hdrhint_cli

tests: $(BUILD_DIR)/CMakeCache.txt
	$(CMAKE) --build $(BUILD_DIR) --config $(CONFIG) $(PARALLEL) --target hdrhint_tests

test: tests
	$(CTEST) --test-dir $(BUILD_DIR) -C $(CONFIG) --output-on-failure

# ---- run -------------------------------------------------------------------------------
run: app
ifeq ($(PLATFORM),macos)
	open "$(APP)"
else
	"$(APP)"
endif

# Every tab in both appearances, plus the busy-queue scenario, at 520 x 720.
screenshots: app
	@mkdir -p "$(SHOTS)"
	@for tab in 0 1 2; do \
	    for look in dark light; do \
	        "$(APP_BINARY)" --screenshot "$(SHOTS)/tab$${tab}_$${look}.png" --tab $$tab --$$look --width 520 --height 720 || exit 1; \
	    done; \
	done
	@"$(APP_BINARY)" --screenshot "$(SHOTS)/queue_busy_dark.png" --tab 0 --dark --scenario 1 --width 520 --height 720
	@echo "Screenshots in $(SHOTS)"

# ---- package / install -----------------------------------------------------------------
package: build
	cd "$(BUILD_DIR)" && $(CPACK) -C $(CONFIG)

dmg zip: package

install: build
	$(CMAKE) --install $(BUILD_DIR) --config $(CONFIG) --prefix "$(PREFIX)"

# The Media Encoder panel for the current user (same code path as the app's
# own "Install panel" button).
panel: app
	"$(APP_BINARY)" --install-panel

unpanel: app
	"$(APP_BINARY)" --uninstall-panel

# ---- assets ------------------------------------------------------------------------------
icon:
	$(PYTHON) scripts/make_icns.py

# ---- clean ---------------------------------------------------------------------------------
clean:
	-$(CMAKE) --build $(BUILD_DIR) --config $(CONFIG) --target clean

distclean:
	rm -rf build build-debug dist
