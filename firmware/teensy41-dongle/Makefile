CXX ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra

.PHONY: all test clean

all: test

test: tests/test_proto tests/test_store
	./tests/test_proto
	./tests/test_store

tests/test_proto: src/amv_catalog_bridge.cpp $(wildcard ../esp32p4-dongle/components/amv_catalog/*.h) ../esp32p4-dongle/components/amv_catalog/amv_catalog.c ../esp32p4-dongle/components/amv_catalog/include/amv_catalog.h tests/test_proto.cpp src/amv_proto_teensy.cpp src/amv_proto_teensy.h ../../common/amv_dongle_proto.h ../../common/amv_octant.h
	@mkdir -p tests
	$(CXX) $(CXXFLAGS) -Isrc -I../esp32p4-dongle/components/amv_catalog/include tests/test_proto.cpp src/amv_proto_teensy.cpp src/amv_catalog_bridge.cpp -o $@

tests/test_store: tests/test_store.cpp src/amv_store_teensy.cpp src/amv_store_teensy.h ../../common/amv_dongle_proto.h
	@mkdir -p tests
	$(CXX) $(CXXFLAGS) -Isrc -I../../common tests/test_store.cpp src/amv_store_teensy.cpp -o $@

clean:
	rm -f tests/test_proto tests/test_store
