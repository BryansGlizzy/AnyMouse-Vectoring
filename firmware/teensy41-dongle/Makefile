CXX ?= g++
CXXFLAGS ?= -std=c++11 -O2 -Wall -Wextra

TEST_TARGETS := tests/test_store
ifneq ($(wildcard tests/test_proto.cpp),)
TEST_TARGETS += tests/test_proto
endif

.PHONY: all test clean

all: test

test: $(TEST_TARGETS)
	@for t in $(TEST_TARGETS); do ./$$t || exit 1; done

tests/test_store: tests/test_store.cpp src/amv_store_teensy.cpp src/amv_store_teensy.h ../../common/amv_dongle_proto.h
	@mkdir -p tests
	$(CXX) $(CXXFLAGS) -Isrc -I../../common tests/test_store.cpp src/amv_store_teensy.cpp -o $@

ifneq ($(wildcard tests/test_proto.cpp),)
tests/test_proto: tests/test_proto.cpp src/amv_proto_teensy.cpp src/amv_proto_teensy.h ../../common/amv_dongle_proto.h ../../common/amv_octant.h
	@mkdir -p tests
	$(CXX) $(CXXFLAGS) -Isrc -I../../common tests/test_proto.cpp src/amv_proto_teensy.cpp -o $@
endif

clean:
	rm -f tests/test_store tests/test_proto
