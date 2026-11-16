CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror
TARGET   = stricta

.PHONY: all clean test

all: $(TARGET)

$(TARGET): stricta.c
	$(CC) $(CFLAGS) stricta.c -o $(TARGET)

test: $(TARGET)
	./build.sh

clean:
	rm -f $(TARGET) *.ll *.o *.out test_bin
