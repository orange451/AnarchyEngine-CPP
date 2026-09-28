# Unix: make && make run
# Windows (from a developer prompt): cmake -S . -B build && cmake --build build --config Release

BUILD_DIR := build

ifeq ($(shell uname),Darwin)
APP_BIN := $(BUILD_DIR)/AnarchyEngine-CPP.app/Contents/MacOS/AnarchyEngine-CPP
else
APP_BIN := $(BUILD_DIR)/AnarchyEngine-CPP
endif

.PHONY: all run test clean

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) --parallel

run: all
	"$(APP_BIN)"

test: all
	"$(BUILD_DIR)/engine-tests"
	"$(BUILD_DIR)/properties-tests"
	"$(BUILD_DIR)/console-tests"
	"$(BUILD_DIR)/studio-tests"
	"$(BUILD_DIR)/mcp-tests"
	"$(BUILD_DIR)/sandbox"

clean:
	rm -rf $(BUILD_DIR)
