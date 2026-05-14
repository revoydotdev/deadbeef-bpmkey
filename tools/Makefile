CFLAGS   ?= -O2 -Wall -Wextra
CXXFLAGS ?= -O2 -Wall -Wextra
PKG_CFLAGS := $(shell pkg-config --cflags aubio)
PKG_LIBS   := $(shell pkg-config --libs aubio libkeyfinder)

all: bpmkey-test

bpmkey-test.o: bpmkey-test.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c $< -o $@

shim.o: ../keyfinder_shim.cpp
	$(CXX) $(CXXFLAGS) -fPIC -c $< -o $@

bpmkey-test: bpmkey-test.o shim.o
	$(CXX) -o $@ $^ $(PKG_LIBS) -lpthread

clean:
	rm -f *.o bpmkey-test

.PHONY: all clean
