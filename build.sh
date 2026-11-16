#!/bin/bash
set -e

echo "=== Building stricta compiler ==="
clang -std=c11 -O2 -Wall -Wextra -Werror stricta.c -o stricta

echo "=== Running test.str ==="
./stricta test.str -o /tmp/stricta_test_bin
/tmp/stricta_test_bin > /dev/null
rm -f /tmp/stricta_test_bin

echo "=== Running tests/*.str ==="
count=0
for f in tests/test_*.str; do
    name=$(basename "$f" .str)
    ./stricta "$f" -o "/tmp/stricta_$name"
    "/tmp/stricta_$name" > /dev/null
    rm -f "/tmp/stricta_$name"
    count=$((count + 1))
done

echo "=== All $count test suites + test.str passed! ==="


