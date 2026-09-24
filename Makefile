# Unix: make && make run
# Windows (from a developer prompt): cmake -S . -B build && cmake --build build --config Release

BUILD_DIR := build

ifeq ($(shell uname),Darwin)
APP_BIN := $(BUILD_DIR)/AnarchyEngine-CPP.app/Contents/MacOS/AnarchyEngine-CPP
else
APP_BIN := $(BUILD_DIR)/AnarchyEngine-CPP
endif

.PHONY: all run clean

all:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) --parallel

run: all
	"$(APP_BIN)"

clean:
	rm -rf $(BUILD_DIR)
