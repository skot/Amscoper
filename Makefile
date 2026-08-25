CXX ?= g++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -pthread
GTK_FLAGS := $(shell pkg-config --cflags --libs gtk+-3.0)
OPENCV_FLAGS := -I/usr/include/opencv5 -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lopencv_videoio

.PHONY: all clean install-user

all: build/amscope-app

build/amscope-app: src/amscope-app.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ $(GTK_FLAGS) $(OPENCV_FLAGS)

install-user: build/amscope-app amscope-microscope.desktop amscope-launcher
	install -Dm755 build/amscope-app $(HOME)/.local/bin/amscope-app
	install -Dm755 amscope-launcher $(HOME)/.local/bin/amscope-launcher
	install -Dm644 amscope-microscope.desktop $(HOME)/.local/share/applications/com.skot.AmScope.desktop
	update-desktop-database $(HOME)/.local/share/applications

clean:
	rm -f build/amscope-app
