CXX = g++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra
PKG = gtk+-3.0 webkit2gtk-4.1
GUI_CFLAGS = $(shell pkg-config --cflags $(PKG))
GUI_LIBS   = $(shell pkg-config --libs $(PKG))

TARGET = architect
SRC = architect.cpp lsp.cpp
# -lutil provides forkpty() for the integrated terminal (PTY-backed shell).
EXTRA_LIBS = -lutil

all: $(TARGET)

$(TARGET): $(SRC) lsp.hpp json.hpp fuzzy.hpp textguard.hpp
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET) $(GUI_CFLAGS) $(GUI_LIBS) -pthread $(EXTRA_LIBS)
	@echo "Build complete: $(TARGET)"
	@ls -lh $(TARGET)

# Unit test for LSP Content-Length framing + a real spawn/round-trip against
# a mock server. Kept out of the *.cpp glob (lives under tests/) so it never
# collides with architect.cpp's main().
test: tests/lsp_test.cpp lsp.cpp lsp.hpp json.hpp fuzzy.hpp textguard.hpp
	$(CXX) $(CXXFLAGS) tests/lsp_test.cpp lsp.cpp -o tests/lsp_test -pthread
	./tests/lsp_test

clean:
	rm -f $(TARGET) tests/lsp_test

.PHONY: all test clean
