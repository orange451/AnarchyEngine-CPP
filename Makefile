# make && make run, on macOS, Linux, and Windows.
# Windows needs GNU make (winget install ezwinports.make) and Visual Studio's C++ tools.

BUILD_DIR := build
CONFIG := Release

ifeq ($(OS),Windows_NT)
# Visual Studio builds every configuration into its own folder, picked at build time.
BIN_DIR := $(BUILD_DIR)/$(CONFIG)
EXE := .exe
APP_BIN := $(BIN_DIR)/AnarchyStudio$(EXE)
else
BIN_DIR := $(BUILD_DIR)
EXE :=
ifeq ($(shell uname),Darwin)
APP_BIN := $(BUILD_DIR)/AnarchyStudio.app/Contents/MacOS/AnarchyStudio
else
APP_BIN := $(BUILD_DIR)/AnarchyStudio
endif
endif

.PHONY: all run test clean

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CONFIG)
	cmake --build $(BUILD_DIR) --config $(CONFIG) --parallel

run: all
	"$(APP_BIN)"

# Every test program CMake registers.
test: all
	cd $(BUILD_DIR) && ctest -C $(CONFIG) --output-on-failure

clean:
	cmake -E rm -rf $(BUILD_DIR)
