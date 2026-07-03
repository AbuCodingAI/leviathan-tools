CC = gcc
CFLAGS = -O3 -s -ffunction-sections -fdata-sections -Wall -Wextra
LDFLAGS = -Wl,--gc-sections
LIBS = $(shell pkg-config --libs webkit2gtk-4.1 gtk+-3.0)

TARGET = scribe
SRC = scribe.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete: $(TARGET)"
	@ls -lh $(TARGET)

$(OBJ): $(SRC)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags webkit2gtk-4.1 gtk+-3.0) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
