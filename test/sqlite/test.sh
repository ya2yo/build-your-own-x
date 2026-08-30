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
  actual_count=$(grep -oF -- "$text" "$output_file" | wc -l)
  if [[ "$actual_count" -ne "$expected_count" ]]; then
    printf 'sqlite test failed: expected %d occurrences of %s, got %d\n' \
      "$expected_count" "$text" "$actual_count" >&2
    cat "$output_file" >&2
    exit 1
  fi
}

run_case() {
  local name=$1
  local input_file=$2
  local output_file=$3
  if ! "$DB" <"$input_file" >"$output_file"; then
    printf 'sqlite test failed: case %s exited unsuccessfully\n' "$name" >&2
    cat "$output_file" >&2
    exit 1
  fi
}

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

# Parser, meta commands, and the normal insert/select execution path.
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
run_case basic "$work_dir/basic.in" "$work_dir/basic.out"
assert_contains "$work_dir/basic.out" 'Executed.'
assert_contains "$work_dir/basic.out" '(1, alice, alice@example.com)'
assert_contains "$work_dir/basic.out" '(2, bob, bob@example.com)'
assert_contains "$work_dir/basic.out" 'ID must be positive.'
assert_contains "$work_dir/basic.out" 'Syntax error. Could not parse statement.'
assert_contains "$work_dir/basic.out" 'String is too long.'
assert_contains "$work_dir/basic.out" "Unrecognized keyword at start of 'update 1 x'."
assert_contains "$work_dir/basic.out" "Unrecognized keyword at start of 'foo'."
assert_contains "$work_dir/basic.out" "unrecognized command '.tables', try again!"
# The empty select produced no row, and the later select produced exactly two.
assert_count "$work_dir/basic.out" 1 '(1, alice, alice@example.com)'
assert_count "$work_dir/basic.out" 1 '(2, bob, bob@example.com)'

# Exact field limits exercise serialization/deserialization across large fields.
username_32=$(printf 'u%.0s' {1..32})
email_255="$(printf 'e%.0s' {1..243})@example.com"
cat >"$work_dir/boundary.in" <<EOF
insert 0 $username_32 $email_255
select
insert 7 ${username_32}x $email_255
insert 8 $username_32 ${email_255}x
.exit
EOF
run_case boundary "$work_dir/boundary.in" "$work_dir/boundary.out"
assert_contains "$work_dir/boundary.out" "(0, $username_32, $email_255)"
assert_count "$work_dir/boundary.out" 2 'String is too long.'
# id=0 is accepted by the current parser and must survive a round trip.
assert_count "$work_dir/boundary.out" 1 '(0, '

# A generated pressure run crosses page boundaries repeatedly and fills all
# 100 table pages. 13 rows fit in one 4096-byte page (ROW_SIZE is 293).
pressure_rows=1301
: >"$work_dir/pressure.in"
for ((id = 0; id < pressure_rows; id++)); do
  printf 'insert %d user%d user%d@example.com\n' "$id" "$id" "$id" >>"$work_dir/pressure.in"
done
printf '.exit\n' >>"$work_dir/pressure.in"
run_case pressure "$work_dir/pressure.in" "$work_dir/pressure.out"
assert_count "$work_dir/pressure.out" 1300 'Executed.'
assert_count "$work_dir/pressure.out" 1 'Error: Table full.'

# Keep a focused page-boundary round trip assertion in addition to the full
# table pressure run (rows 13/14 and 27/28 land on adjacent pages).
cat >"$work_dir/pages.in" <<'EOF'
insert 13 page13 page13@example.com
insert 14 page14 page14@example.com
insert 27 page27 page27@example.com
insert 28 page28 page28@example.com
select
.exit
EOF
run_case pages "$work_dir/pages.in" "$work_dir/pages.out"
for row in \
  '(13, page13, page13@example.com)' \
  '(14, page14, page14@example.com)' \
  '(27, page27, page27@example.com)' \
  '(28, page28, page28@example.com)'; do
  assert_contains "$work_dir/pages.out" "$row"
done

printf 'sqlite: parser, meta commands, round trips, page boundaries, and %d-row pressure test passed\n' "$pressure_rows"
