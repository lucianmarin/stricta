CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror
TARGET   = turbo

.PHONY: all clean test

all: $(TARGET)

$(TARGET): turbo.c
	$(CC) $(CFLAGS) turbo.c -o $(TARGET)

test: $(TARGET)
	./build.sh

clean:
	rm -f $(TARGET) *.ll *.o *.out test_bin
