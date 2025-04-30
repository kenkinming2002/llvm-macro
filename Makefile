ifeq ($(origin CXX),default)
CXX = clang++
endif
CXXFLAGS ?= -g

libplugin.so: plugin.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -shared -fPIC

macro.ll: macro.cpp macro.h
	clang++ $(CXXFLAGS) -o $@ $< -Xclang -disable-O0-optnone -S -emit-llvm

macro.bc: macro.cpp macro.h
	clang++ $(CXXFLAGS) -o $@ $< -Xclang -disable-O0-optnone -c -emit-llvm

test.ll: test.cpp libplugin.so macro.bc
	clang++ $(CXXFLAGS) -o $@ $< -fplugin=./libplugin.so -fpass-plugin=./libplugin.so -mllvm -macro -mllvm macro.bc -S -emit-llvm

test: test.cpp libplugin.so macro.bc
	clang++ $(CXXFLAGS) -o $@ $< -fplugin=./libplugin.so -fpass-plugin=./libplugin.so -mllvm -macro -mllvm macro.bc

.PHONY: all
all: macro.ll macro.bc test.ll test

.PHONY: clean
clean:
	- rm -f libplugin.so
	- rm -f macro.ll
	- rm -f macro.bc
	- rm -f test.ll
	- rm -f test

