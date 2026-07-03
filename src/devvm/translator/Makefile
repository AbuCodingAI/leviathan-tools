CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -march=x86-64 -I.. -std=c++20 -fPIC

# Common source files
COMMON_SRCS = x86_disasm.c
COMMON_OBJS = $(COMMON_SRCS:.c=.o)

# ELF→.dev compiler
ELF_SRCS = elf2dev.c elf_parser.c $(COMMON_SRCS)
ELF_OBJS = $(ELF_SRCS:.c=.o) ../packer/devpack.o

# PE→.dev compiler
PE_SRCS = pe2dev.c pe_parser.c $(COMMON_SRCS)
PE_OBJS = $(PE_SRCS:.c=.o) ../packer/devpack.o

# Build both translators
all: elf2dev pe2dev

elf2dev: $(ELF_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^
	strip $@
	@echo "[✓] Built: elf2dev (ELF→.dev) - $(shell du -h $@ | awk '{print $$1}')"

pe2dev: $(PE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^
	strip $@
	@echo "[✓] Built: pe2dev (PE→.dev) - $(shell du -h $@ | awk '{print $$1}')"

%.o: %.c
	$(CXX) $(CXXFLAGS) -c -o $@ $<

../packer/devpack.o: ../packer/devpack.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f *.o ../packer/devpack.o elf2dev pe2dev

install: elf2dev pe2dev
	cp elf2dev ../../bin/elf2dev
	cp pe2dev ../../bin/pe2dev
	chmod +x ../../bin/elf2dev ../../bin/pe2dev
	@echo "[✓] Installed elf2dev and pe2dev to bin/"

test: elf2dev
	@echo "Testing elf2dev with test binary…"
	./elf2dev ../../bin/test_devvm test.dev
	@echo "[✓] ELF→.dev test complete"
	@du -h test.dev

.PHONY: all clean install test
