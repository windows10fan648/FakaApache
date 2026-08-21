CXX ?= g++
MINGW_CXX ?= x86_64-w64-mingw32-g++-posix
MINGW_LDFLAGS ?= -static -static-libgcc -static-libstdc++ -lws2_32

CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread
TARGET ?= fakaapache

ifeq ($(OS),Windows_NT)
TARGET := fakaapache.exe
LDLIBS += -lws2_32
endif

.PHONY: all mingw test clean

all: $(TARGET)

$(TARGET): webserver.cpp
	$(CXX) $(CXXFLAGS) webserver.cpp -o $@ $(LDLIBS)

# Cross-compile a 64-bit Windows executable from a POSIX host.
mingw: webserver.cpp
	$(MINGW_CXX) $(CXXFLAGS) webserver.cpp -o fakaapache.exe $(MINGW_LDFLAGS)

test: fakaapache
	./tests/integration_test.sh

clean:
	rm -f fakaapache fakaapache.exe
