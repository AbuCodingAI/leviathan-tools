CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -march=x86-64 -I.. -std=c++20 -fPIC

# JIT→C library
LIB = libjit_to_c.a
LIB_SRCS = jit_to_c.cpp
LIB_OBJS = $(LIB_SRCS:.cpp=.o)

# Test executable
TEST_BIN = test_jit_to_c
TEST_SRCS = test_jit_to_c.cpp
TEST_OBJS = $(TEST_SRCS:.cpp=.o)

all: $(LIB) $(TEST_BIN)

$(LIB): $(LIB_OBJS)
	ar rcs $@ $^
	@echo "[✓] Built: $(LIB)"

$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJS) $(LIB)
	@echo "[✓] Built: $(TEST_BIN)"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(LIB_OBJS) $(TEST_OBJS) $(LIB) $(TEST_BIN)

test: $(TEST_BIN)
	@echo "════════════════════════════════════════════════════════"
	@echo "Testing JIT→C pipeline with /tmp/test.dev..."
	@echo "════════════════════════════════════════════════════════"
	@echo ""
	@if [ -f /tmp/test.dev ]; then \
		./$(TEST_BIN) /tmp/test.dev; \
	else \
		echo "ERROR: /tmp/test.dev not found!"; \
		echo "Build test.dev first using: cd /media/abu/CoderDrive/LeviathanOS/src/devvm/translator && ./elf2dev /tmp/test.acb /tmp/test.dev"; \
		exit 1; \
	fi

.PHONY: all clean test
