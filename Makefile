CXXFLAGS ?= -g

LLVM_CFLAGS = $(shell llvm-config --cflags)
ifeq ($(shell llvm-config --has-rtti),NO)
LLVM_CFLAGS += -fno-rtti
endif

TEST_CXX ?= clang++
TEST_CXXFLAGS ?= -g

libplugin.so: plugin.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LLVM_CFLAGS) -shared -fPIC

macro.ll: macro.cpp macro.h
	$(TEST_CXX) $(TEST_CXXFLAGS) -o $@ $< -Xclang -disable-O0-optnone -S -emit-llvm

macro.bc: macro.cpp macro.h
	$(TEST_CXX) $(TEST_CXXFLAGS) -o $@ $< -Xclang -disable-O0-optnone -c -emit-llvm

test.ll: test.cpp libplugin.so macro.bc
	$(TEST_CXX) $(TEST_CXXFLAGS) -o $@ $< -fplugin=./libplugin.so -fpass-plugin=./libplugin.so -mllvm -macro -mllvm macro.bc -S -emit-llvm

test: test.cpp libplugin.so macro.bc
	$(TEST_CXX) $(TEST_CXXFLAGS) -o $@ $< -fplugin=./libplugin.so -fpass-plugin=./libplugin.so -mllvm -macro -mllvm macro.bc

.PHONY: all
all: macro.ll macro.bc test.ll test

.PHONY: clean-test
clean-test:
	- rm -f macro.ll
	- rm -f macro.bc
	- rm -f test.ll
	- rm -f test

.PHONY: clean
clean: clean-test
	- rm -f libplugin.so

