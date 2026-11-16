CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/stricta
TARGET   = stricta

.PHONY: all clean test install uninstall

all: $(TARGET)

$(TARGET): stricta.c
	$(CC) $(CFLAGS) -DSTRICTA_DATA_DIR=\"$(DATADIR)\" stricta.c -o $(TARGET)

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

install:
	$(CC) $(CFLAGS) -DSTRICTA_DATA_DIR=\"$(DATADIR)\" stricta.c -o $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(DATADIR)"
	install -m 755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	install -m 644 runtime.h runtime.c "$(DESTDIR)$(DATADIR)/"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(TARGET)"
	rm -rf "$(DESTDIR)$(DATADIR)"

clean:
	rm -f $(TARGET) *.ll *.o *.out test_bin
