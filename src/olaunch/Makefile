CC ?= gcc
CFLAGS ?= -O2 -s -Wall -Wextra $(shell pkg-config --cflags gtk4)
LIBS := $(shell pkg-config --libs gtk4) -lm

olaunch: olaunch.c expr.h units.h
	$(CC) $(CFLAGS) olaunch.c -o olaunch $(LIBS)

clean:
	rm -f olaunch

.PHONY: clean
