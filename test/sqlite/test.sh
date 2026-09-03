#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

./c.sh build --bin sqlite >/dev/null
DB="target/c/sqlite"

assert_contains() {
  local output_file=$1
  local expected=$2
  if ! grep -Fq -- "$expected" "$output_file"; then
    printf 'sqlite test failed: missing output: %s\n' "$expected" >&2
    cat "$output_file" >&2
    exit 1
  fi
}

assert_count() {
  local output_file=$1
  local expected_count=$2
  local text=$3
  local actual_count
  actual_count=$(awk -v expected="$text" 'index($0, expected) { count++ } END { print count + 0 }' "$output_file")
  if [[ "$actual_count" -ne "$expected_count" ]]; then
    printf 'sqlite test failed: expected %d occurrences of %s, got %d\n' \
      "$expected_count" "$text" "$actual_count" >&2
    cat "$output_file" >&2
    exit 1
  fi
}

run_case() {
  local name=$1
  local db_file=$2
  local input_file=$3
  local output_file=$4
  if ! "$DB" "$db_file" <"$input_file" >"$output_file"; then
    printf 'sqlite test failed: case %s exited unsuccessfully\n' "$name" >&2
    cat "$output_file" >&2
    exit 1
  fi
}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

# Empty select, successful INSERT/SELECT, parser errors, and .exit file close.
cat >"$work_dir/basic.in" <<'EOF'
select
insert 1 alice alice@example.com
insert 2 bob bob@example.com
select
insert -1 negative negative@example.com
insert 3 missing_email
insert 4 this_username_is_longer_than_thirty_two_chars long@example.com
insert 5 carol carol@example.com extra_tokens_are_ignored
update 1 x
foo
.tables
.exit
EOF
run_case basic "$work_dir/basic.db" "$work_dir/basic.in" "$work_dir/basic.out"
assert_contains "$work_dir/basic.out" 'Executed.'
assert_contains "$work_dir/basic.out" '(1, alice, alice@example.com)'
assert_contains "$work_dir/basic.out" '(2, bob, bob@example.com)'
assert_contains "$work_dir/basic.out" 'ID must be positive.'
assert_contains "$work_dir/basic.out" 'Syntax error. Could not parse statement.'
assert_contains "$work_dir/basic.out" 'String is too long.'
assert_contains "$work_dir/basic.out" "Unrecognized keyword at start of 'update 1 x'."
assert_contains "$work_dir/basic.out" "Unrecognized keyword at start of 'foo'."
assert_contains "$work_dir/basic.out" "unrecognized command '.tables', try again!"
assert_count "$work_dir/basic.out" 1 '(1, alice, alice@example.com)'
assert_count "$work_dir/basic.out" 1 '(2, bob, bob@example.com)'
[[ -f "$work_dir/basic.db" ]] || { echo 'sqlite test failed: database was not created' >&2; exit 1; }

# Reopen the same file: rows must survive close/open and be selectable again.
cat >"$work_dir/reopen.in" <<'EOF'
select
.exit
EOF
run_case reopen "$work_dir/basic.db" "$work_dir/reopen.in" "$work_dir/reopen.out"
assert_count "$work_dir/reopen.out" 1 '(1, alice, alice@example.com)'
assert_count "$work_dir/reopen.out" 1 '(2, bob, bob@example.com)'

# Appending after reopen must preserve existing rows and write the new row.
cat >"$work_dir/append.in" <<'EOF'
insert 3 carol carol@example.com
.exit
EOF
run_case append "$work_dir/basic.db" "$work_dir/append.in" "$work_dir/append.out"
run_case append-reopen "$work_dir/basic.db" "$work_dir/reopen.in" "$work_dir/append-reopen.out"
assert_count "$work_dir/append-reopen.out" 1 '(1, alice, alice@example.com)'
assert_count "$work_dir/append-reopen.out" 1 '(2, bob, bob@example.com)'
assert_count "$work_dir/append-reopen.out" 1 '(3, carol, carol@example.com)'

# Exact serialized field limits, id zero, and the first page boundary.
username_32=$(printf 'u%.0s' {1..32})
email_255="$(printf 'e%.0s' {1..243})@example.com"
cat >"$work_dir/boundary.in" <<EOF
insert 0 $username_32 $email_255
insert 13 page13 page13@example.com
insert 14 page14 page14@example.com
insert 27 page27 page27@example.com
insert 28 page28 page28@example.com
select
insert 7 ${username_32}x $email_255
insert 8 $username_32 ${email_255}x
.exit
EOF
run_case boundary "$work_dir/boundary.db" "$work_dir/boundary.in" "$work_dir/boundary.out"
assert_contains "$work_dir/boundary.out" "(0, $username_32, $email_255)"
for row in \
  '(13, page13, page13@example.com)' \
  '(14, page14, page14@example.com)' \
  '(27, page27, page27@example.com)' \
  '(28, page28, page28@example.com)'; do
  assert_contains "$work_dir/boundary.out" "$row"
done
assert_count "$work_dir/boundary.out" 2 'String is too long.'
assert_count "$work_dir/boundary.out" 1 '(0, '

# Non-sequential inserts exercise middle-leaf splits, separator updates, and
# lookup through an internal root.  The output must remain sorted even though
# the insertion order is deliberately shuffled.
cat >"$work_dir/random.in" <<'EOF'
insert 50 user50 user50@example.com
insert 10 user10 user10@example.com
insert 90 user90 user90@example.com
insert 30 user30 user30@example.com
insert 70 user70 user70@example.com
insert 20 user20 user20@example.com
insert 80 user80 user80@example.com
insert 40 user40 user40@example.com
insert 60 user60 user60@example.com
insert 0 user0 user0@example.com
insert 50 duplicate duplicate@example.com
select
.btree
.exit
EOF
run_case random "$work_dir/random.db" "$work_dir/random.in" "$work_dir/random.out"
assert_count "$work_dir/random.out" 1 'Error: Duplicate key.'
for id in 0 10 20 30 40 50 60 70 80 90; do
  assert_count "$work_dir/random.out" 1 "($id, user$id, user$id@example.com)"
done
assert_count "$work_dir/random.out" 1 'Tree: '

# Pressure test: insert substantially more rows than the current pager can
# hold.  Do not hard-code a capacity: the root/internal-node layout may evolve,
# while these invariants must continue to hold.
pressure_rows=5000
: >"$work_dir/pressure.in"
for ((id = 0; id < pressure_rows; id++)); do
  printf 'insert %d user%d user%d@example.com\n' "$id" "$id" "$id" >>"$work_dir/pressure.in"
done
printf 'select\n.exit\n' >>"$work_dir/pressure.in"
pressure_start=$(date +%s%N)
run_case pressure "$work_dir/pressure.db" "$work_dir/pressure.in" "$work_dir/pressure.out"
pressure_end=$(date +%s%N)
pressure_elapsed_ns=$((pressure_end - pressure_start))
pressure_elapsed_ms=$((pressure_elapsed_ns / 1000000))

assert_contains "$work_dir/pressure.out" 'Error: Table full.'
assert_count "$work_dir/pressure.out" 1 '(0, user0, user0@example.com)'

# Find the rows printed by SELECT and verify they form a contiguous, sorted
# prefix.  This catches lost rows, duplicate rows, corrupted values, and bad
# leaf-link traversal without depending on an implementation-specific limit.
mapfile -t pressure_rows_out < <(sed 's/^db > //' "$work_dir/pressure.out" | awk '/^\([0-9]+, user[0-9]+, user[0-9]+@example\.com\)$/ { print }')
row_count=${#pressure_rows_out[@]}
if ((row_count == 0)); then
  printf 'sqlite test failed: pressure SELECT returned no rows\n' >&2
  exit 1
fi
for ((index = 0; index < row_count; index++)); do
  expected="($index, user$index, user$index@example.com)"
  if [[ "${pressure_rows_out[index]}" != "$expected" ]]; then
    printf 'sqlite test failed: pressure row %d was %s, expected %s\n' \
      "$index" "${pressure_rows_out[index]}" "$expected" >&2
    exit 1
  fi
done

# The attempted final id must not be present if the table became full before
# the end of the input stream.
if ((row_count >= pressure_rows)); then
  printf 'sqlite test failed: pressure did not reach table capacity\\n' >&2
  exit 1
fi
last_id=$((row_count - 1))
assert_count "$work_dir/pressure.out" 1 "($last_id, user$last_id, user$last_id@example.com)"

# Reopen the pressure database and verify that every row selected before close
# is still present after page-backed persistence.
run_case pressure-reopen "$work_dir/pressure.db" "$work_dir/reopen.in" "$work_dir/pressure-reopen.out"
reopened_count=$(sed 's/^db > //' "$work_dir/pressure-reopen.out" | awk '/^\([0-9]+, user[0-9]+, user[0-9]+@example\.com\)$/ { count++ } END { print count + 0 }')
[[ "$reopened_count" -eq "$row_count" ]] || {
  printf 'sqlite test failed: expected %d rows after reopen, got %d\n' \
    "$row_count" "$reopened_count" >&2
  exit 1
}
assert_count "$work_dir/pressure-reopen.out" 1 '(0, user0, user0@example.com)'
assert_count "$work_dir/pressure-reopen.out" 1 "($last_id, user$last_id, user$last_id@example.com)"

# Pages are always flushed in PAGE_SIZE units and must stay within the pager's
# configured maximum.  This checks page I/O without assuming an exact capacity.
actual_size=$(stat -c '%s' "$work_dir/pressure.db")
((actual_size > 0 && actual_size % 4096 == 0)) || {
  printf 'sqlite test failed: database size %d is not page-aligned\n' \
    "$actual_size" >&2
  exit 1
}
max_size=$((400 * 4096))
((actual_size <= max_size)) || {
  printf 'sqlite test failed: database size %d exceeds pager limit %d\n' \
    "$actual_size" "$max_size" >&2
  exit 1
}
printf 'sqlite pressure: %d attempted inserts, %d persisted rows, elapsed %d ms\n' \
  "$pressure_rows" "$row_count" "$pressure_elapsed_ms"
printf 'sqlite: insert/select, duplicate keys, middle splits, boundaries, persistence, page I/O, and pressure tests passed\n'
