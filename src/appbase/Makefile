CC = gcc
CFLAGS = -O3 -s -ffunction-sections -fdata-sections -Wall -Wextra
LDFLAGS = -Wl,--gc-sections
LIBS = $(shell pkg-config --libs webkit2gtk-4.1 gtk+-3.0) -lsqlite3

TARGET = appbase
SRC = appbase.c datac.c
OBJ = appbase.o datac.o

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete: $(TARGET)"
	@ls -lh $(TARGET)

appbase.o: appbase.c datac.h
	$(CC) $(CFLAGS) $(shell pkg-config --cflags webkit2gtk-4.1 gtk+-3.0) -c appbase.c -o $@

datac.o: datac.c datac.h
	$(CC) $(CFLAGS) -c datac.c -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
