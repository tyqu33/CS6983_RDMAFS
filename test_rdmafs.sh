#!/bin/bash
#
# test_rdmafs.sh - Automated test for RDMA-FS
#
# Usage:
#   Terminal 1:  ./rdmafs -machine 0 -f /tmp/mnt0
#   Terminal 2:  ./rdmafs -machine 1 -f /tmp/mnt1
#   Terminal 3:  bash test_rdmafs.sh
#

MNT0="/tmp/mnt0"
MNT1="/tmp/mnt1"
PASS=0
FAIL=0

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

check() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo -e "  ${GREEN}PASS${NC}: $desc"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $desc"
        echo -e "       expected: $(echo "$expected" | head -3)"
        echo -e "       actual:   $(echo "$actual" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

check_contains() {
    local desc="$1"
    local needle="$2"
    local haystack="$3"
    if echo "$haystack" | grep -q "$needle"; then
        echo -e "  ${GREEN}PASS${NC}: $desc"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $desc"
        echo -e "       expected to contain: $needle"
        echo -e "       actual: $(echo "$haystack" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

check_not_contains() {
    local desc="$1"
    local needle="$2"
    local haystack="$3"
    if ! echo "$haystack" | grep -q "$needle"; then
        echo -e "  ${GREEN}PASS${NC}: $desc"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $desc"
        echo -e "       should NOT contain: $needle"
        FAIL=$((FAIL + 1))
    fi
}

# Pre-check: both mount points accessible
echo -e "${YELLOW}=== Pre-check ===${NC}"
if ! ls "$MNT0" > /dev/null 2>&1; then
    echo -e "${RED}ERROR: $MNT0 not mounted. Start machine 0 first.${NC}"
    exit 1
fi
if ! ls "$MNT1" > /dev/null 2>&1; then
    echo -e "${RED}ERROR: $MNT1 not mounted. Start machine 1 first.${NC}"
    exit 1
fi
echo "  Both mount points accessible."
echo ""

# ============================================================
echo -e "${YELLOW}=== Test 1: Basic create and read ===${NC}"
echo "hello world" > "$MNT0/test1.txt"
result=$(cat "$MNT0/test1.txt")
check "write and read back" "hello world" "$result"

# ============================================================
echo -e "${YELLOW}=== Test 2: Multiple files ===${NC}"
echo "file_a" > "$MNT0/a.txt"
echo "file_b" > "$MNT0/b.txt"
echo "file_c" > "$MNT0/c.txt"
check "read a.txt" "file_a" "$(cat "$MNT0/a.txt")"
check "read b.txt" "file_b" "$(cat "$MNT0/b.txt")"
check "read c.txt" "file_c" "$(cat "$MNT0/c.txt")"

listing=$(ls "$MNT0")
check_contains "ls shows a.txt" "a.txt" "$listing"
check_contains "ls shows b.txt" "b.txt" "$listing"
check_contains "ls shows c.txt" "c.txt" "$listing"

# ============================================================
echo -e "${YELLOW}=== Test 3: Overwrite ===${NC}"
echo "original" > "$MNT0/overwrite.txt"
check "before overwrite" "original" "$(cat "$MNT0/overwrite.txt")"
echo "modified content here" > "$MNT0/overwrite.txt"
check "after overwrite" "modified content here" "$(cat "$MNT0/overwrite.txt")"

# ============================================================
echo -e "${YELLOW}=== Test 4: Subdirectory ===${NC}"
mkdir -p "$MNT0/subdir"
echo "nested" > "$MNT0/subdir/deep.txt"
check "read nested file" "nested" "$(cat "$MNT0/subdir/deep.txt")"
check_contains "ls subdir" "deep.txt" "$(ls "$MNT0/subdir")"

# ============================================================
echo -e "${YELLOW}=== Test 5: Nested subdirectories ===${NC}"
mkdir -p "$MNT0/level1"
mkdir -p "$MNT0/level1/level2"
echo "deep nested" > "$MNT0/level1/level2/file.txt"
check "read 2-level nested file" "deep nested" "$(cat "$MNT0/level1/level2/file.txt")"

# ============================================================
echo -e "${YELLOW}=== Test 6: Delete file (unlink) ===${NC}"
echo "delete me" > "$MNT0/todelete.txt"
check "file exists before delete" "delete me" "$(cat "$MNT0/todelete.txt")"
rm "$MNT0/todelete.txt"
listing=$(ls "$MNT0")
check_not_contains "file gone after delete" "todelete.txt" "$listing"

# ============================================================
echo -e "${YELLOW}=== Test 7: Delete empty directory (rmdir) ===${NC}"
mkdir -p "$MNT0/emptydir"
check_contains "dir exists" "emptydir" "$(ls "$MNT0")"
rmdir "$MNT0/emptydir"
check_not_contains "dir removed" "emptydir" "$(ls "$MNT0")"

# ============================================================
echo -e "${YELLOW}=== Test 8: Cross-machine read (machine 0 writes, machine 1 reads) ===${NC}"
echo "from machine 0" > "$MNT0/cross0.txt"
result=$(cat "$MNT1/cross0.txt")
check "machine 1 reads machine 0's file" "from machine 0" "$result"

listing=$(ls "$MNT1")
check_contains "machine 1 sees test1.txt" "test1.txt" "$listing"
check_contains "machine 1 sees a.txt" "a.txt" "$listing"

# ============================================================
echo -e "${YELLOW}=== Test 9: Cross-machine write (machine 1 writes, machine 0 reads) ===${NC}"
echo "from machine 1" > "$MNT1/cross1.txt"
result=$(cat "$MNT0/cross1.txt")
check "machine 0 reads machine 1's file" "from machine 1" "$result"

# ============================================================
echo -e "${YELLOW}=== Test 10: Both machines see same directory listing ===${NC}"
listing0=$(ls "$MNT0" | sort)
listing1=$(ls "$MNT1" | sort)
check "directory listings match" "$listing0" "$listing1"

# ============================================================
echo -e "${YELLOW}=== Test 11: Cross-machine subdirectory access ===${NC}"
mkdir -p "$MNT0/shared_dir"
echo "shared content" > "$MNT0/shared_dir/shared.txt"
result=$(cat "$MNT1/shared_dir/shared.txt")
check "machine 1 reads machine 0's subdirectory file" "shared content" "$result"

# ============================================================
echo -e "${YELLOW}=== Test 12: Larger write (multi-block) ===${NC}"
# Generate 10KB of data (spans 3 blocks)
python3 -c "print('A' * 10000)" > "$MNT0/large.txt"
python3 -c "print('A' * 10000)" > /tmp/expected_large.txt
result=$(cat "$MNT0/large.txt")
expected=$(python3 -c "print('A' * 10000)")
check "10KB file write and read" "$expected" "$result"

# ============================================================
echo -e "${YELLOW}=== Test 13: Cross-machine large file ===${NC}"
result=$(cat "$MNT1/large.txt")
check "machine 1 reads 10KB file from machine 0" "$expected" "$result"

# ============================================================
echo -e "${YELLOW}=== Test 14: File size correctness ===${NC}"
echo -n "exactly20characters!" > "$MNT0/sized.txt"
size=$(stat -c %s "$MNT0/sized.txt" 2>/dev/null || stat -f %z "$MNT0/sized.txt" 2>/dev/null)
check "file size is 20 bytes" "20" "$size"

# ============================================================
echo -e "${YELLOW}=== Test 15: Empty file ===${NC}"
touch "$MNT0/empty.txt"
size=$(stat -c %s "$MNT0/empty.txt" 2>/dev/null || stat -f %z "$MNT0/empty.txt" 2>/dev/null)
check "empty file size is 0" "0" "$size"
result=$(cat "$MNT0/empty.txt")
check "empty file reads empty" "" "$result"

# ============================================================
echo -e "${YELLOW}=== Test 16: Machine 1 deletes, machine 0 sees it ===${NC}"
echo "will be deleted" > "$MNT1/m1delete.txt"
check_contains "machine 0 sees file before delete" "m1delete.txt" "$(ls "$MNT0")"
rm "$MNT1/m1delete.txt"
check_not_contains "machine 0 sees file removed" "m1delete.txt" "$(ls "$MNT0")"

# ============================================================
# Summary
echo ""
echo "======================================"
echo -e "  ${GREEN}PASSED: $PASS${NC}"
echo -e "  ${RED}FAILED: $FAIL${NC}"
echo "  TOTAL:  $((PASS + FAIL))"
echo "======================================"

if [ $FAIL -eq 0 ]; then
    echo -e "  ${GREEN}ALL TESTS PASSED!${NC}"
else
    echo -e "  ${RED}SOME TESTS FAILED${NC}"
fi
