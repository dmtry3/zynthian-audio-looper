# Только офлайн-тесты ядра. Плагин собирается скриптом scripts/build.sh.
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti

CORE_H := $(wildcard core/*.h)
CORE_CPP := $(wildcard core/*.cpp)

.PHONY: test clean

test: build_test/test_core build_test/test_looper
	@mkdir -p tests/out
	./build_test/test_core
	./build_test/test_looper

build_test/test_core: tests/test_core.cpp $(CORE_H)
	@mkdir -p build_test
	$(CXX) $(CXXFLAGS) -o $@ tests/test_core.cpp -lm

build_test/test_looper: tests/test_looper.cpp $(CORE_H) $(CORE_CPP)
	@mkdir -p build_test
	$(CXX) $(CXXFLAGS) -o $@ tests/test_looper.cpp $(CORE_CPP) -lm

clean:
	rm -rf build build_test tests/out
