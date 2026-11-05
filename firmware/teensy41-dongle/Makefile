CXX ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra
.PHONY: test clean
test: tests/test_proto
	./tests/test_proto
tests/test_proto: tests/test_proto.cpp src/amv_proto_teensy.cpp src/amv_proto_teensy.h ../../common/amv_dongle_proto.h ../../common/amv_octant.h
	$(CXX) $(CXXFLAGS) -Isrc tests/test_proto.cpp src/amv_proto_teensy.cpp -o $@
clean:
	rm -f tests/test_proto
