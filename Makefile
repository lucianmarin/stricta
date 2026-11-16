CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror
TARGET   = stricta

.PHONY: all clean test

all: $(TARGET)

$(TARGET): stricta.c
	$(CC) $(CFLAGS) stricta.c -o $(TARGET)

test: $(TARGET)
	@set -e; \
	echo "=== Running test.str ==="; \
	./$(TARGET) test.str -o /tmp/stricta_test_bin; \
	/tmp/stricta_test_bin > /dev/null; \
	rm -f /tmp/stricta_test_bin; \
	echo "=== Running tests/*.str ==="; \
	count=0; \
	for f in tests/test_*.str; do \
		name=$$(basename "$$f" .str); \
		./$(TARGET) "$$f" -o "/tmp/stricta_$$name"; \
		"/tmp/stricta_$$name" > /dev/null; \
		rm -f "/tmp/stricta_$$name"; \
		count=$$((count + 1)); \
	done; \
	echo "=== All $$count test suites + test.str passed! ==="

clean:
	rm -f $(TARGET) *.ll *.o *.out test_bin
