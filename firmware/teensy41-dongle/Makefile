CXX ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra

.PHONY: all test clean

all: test

test: tests/test_proto tests/test_store
	./tests/test_proto
	./tests/test_store

tests/test_proto: tests/test_proto.cpp src/amv_proto_teensy.cpp src/amv_proto_teensy.h ../../common/amv_dongle_proto.h ../../common/amv_octant.h
	@mkdir -p tests
	$(CXX) $(CXXFLAGS) -Isrc tests/test_proto.cpp src/amv_proto_teensy.cpp -o $@

tests/test_store: tests/test_store.cpp src/amv_store_teensy.cpp src/amv_store_teensy.h ../../common/amv_dongle_proto.h
	@mkdir -p tests
	$(CXX) $(CXXFLAGS) -Isrc -I../../common tests/test_store.cpp src/amv_store_teensy.cpp -o $@

clean:
	rm -f tests/test_proto tests/test_store
