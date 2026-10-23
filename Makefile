CXX ?= g++

CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread -DFAKAAPACHE_WITH_OPENSSL
OPENSSL_LDFLAGS ?= -lssl -lcrypto -lz
TARGET ?= fakaapache

ifeq ($(OS),Windows_NT)
TARGET := fakaapache.exe
LDLIBS += -lws2_32
endif

.PHONY: all test clean

all: $(TARGET)

$(TARGET): webserver.cpp
	$(CXX) $(CXXFLAGS) webserver.cpp -o $@ $(LDLIBS) $(OPENSSL_LDFLAGS)

test: fakaapache
	./tests/integration_test.sh

clean:
	rm -f fakaapache fakaapache.exe
