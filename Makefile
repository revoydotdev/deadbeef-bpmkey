PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib
PLUGDIR ?= $(LIBDIR)/deadbeef
DESTDIR ?=

CFLAGS   ?= -O2 -Wall -Wextra
CXXFLAGS ?= -O2 -Wall -Wextra
CFLAGS   += -fPIC
CXXFLAGS += -fPIC

PKG_CFLAGS := $(shell pkg-config --cflags aubio)
PKG_LIBS   := $(shell pkg-config --libs aubio libkeyfinder)

LIBS := $(PKG_LIBS) -lpthread

all: bpmkey.so

bpmkey.o: bpmkey.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c $< -o $@

keyfinder_shim.o: keyfinder_shim.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

bpmkey.so: bpmkey.o keyfinder_shim.o
	$(CXX) -shared -o $@ $^ $(LDFLAGS) $(LIBS)

install: bpmkey.so
	install -Dm0755 bpmkey.so $(DESTDIR)$(PLUGDIR)/bpmkey.so

uninstall:
	rm -f $(DESTDIR)$(PLUGDIR)/bpmkey.so

clean:
	rm -f *.o bpmkey.so

.PHONY: all install uninstall clean
