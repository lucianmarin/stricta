#!/bin/bash
set -e

echo "=== Building turbo compiler ==="
clang -std=c11 -O2 -Wall -Wextra -Werror turbo.c -o turbo

echo "=== Running test.turbo ==="
./turbo test.turbo -o /tmp/turbo_test_bin
/tmp/turbo_test_bin > /dev/null
rm -f /tmp/turbo_test_bin

echo "=== Running tests/*.turbo ==="
count=0
for f in tests/test_*.turbo; do
    name=$(basename "$f" .turbo)
    ./turbo "$f" -o "/tmp/turbo_$name"
    "/tmp/turbo_$name" > /dev/null
    rm -f "/tmp/turbo_$name"
    count=$((count + 1))
done

echo "=== All $count test suites + test.turbo passed! ==="


