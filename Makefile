CXXFLAGS ?= -g

LLVM_CFLAGS = $(shell llvm-config --cflags)
ifeq ($(shell llvm-config --has-rtti),NO)
LLVM_CFLAGS += -fno-rtti
endif

TEST_CXX ?= clang++
TEST_CXXFLAGS ?= -g

libmacro.so: macro.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LLVM_CFLAGS) -shared -fPIC

test-macro.ll: test-macro.cpp macro.h
	$(TEST_CXX) $(TEST_CXXFLAGS) -o $@ $< -Xclang -disable-O0-optnone -S -emit-llvm

test-macro.bc: test-macro.cpp macro.h
	$(TEST_CXX) $(TEST_CXXFLAGS) -o $@ $< -Xclang -disable-O0-optnone -c -emit-llvm

test.ll: test.cpp libmacro.so test-macro.bc
	$(TEST_CXX) $(TEST_CXXFLAGS) -o $@ $< -fplugin=./libmacro.so -fpass-plugin=./libmacro.so -mllvm -macro -mllvm test-macro.bc -S -emit-llvm

test: test.cpp libmacro.so test-macro.bc
	$(TEST_CXX) $(TEST_CXXFLAGS) -o $@ $< -fplugin=./libmacro.so -fpass-plugin=./libmacro.so -mllvm -macro -mllvm test-macro.bc

.PHONY: all
all: test-macro.ll test-macro.bc test.ll test

.PHONY: clean-test
clean-test:
	- rm -f test-macro.ll
	- rm -f test-macro.bc
	- rm -f test.ll
	- rm -f test

.PHONY: clean
clean: clean-test
	- rm -f libmacro.so

