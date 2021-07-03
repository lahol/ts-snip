CC = gcc
LD = gcc
PKG_CONFIG = pkg-config
CFLAGS += -Wall -g -D_FILE_OFFSET_BITS=64 `$(PKG_CONFIG) --cflags glib-2.0 gtk+-3.0 gdk-3.0 json-glib-1.0 x11 libavcodec libswscale`
LIBS += -ldvbpsi -ltsanalyze `$(PKG_CONFIG) --libs glib-2.0 gtk+-3.0 gdk-3.0 json-glib-1.0 x11 libavcodec libavutil libswscale` -lmagic
RM ?= rm

PREFIX := /usr

all: ts-snip

tss_SRC := $(wildcard *.c)
tss_OBJ := $(tss_SRC:.c=.o)
tss_HEADERS := $(wildcard *.h)

ts-snip: $(tss_OBJ)
	$(LD) -o $@ $^ $(LIBS)

%.o: %.c $(tss_HEADERS)
	$(CC) -I. $(CFLAGS) -c -o $@ $<

install: ts-snip
	install ts-snip $(PREFIX)/bin

clean:
	$(RM) ts-snip $(tss_OBJ)

.PHONY: all clean install
