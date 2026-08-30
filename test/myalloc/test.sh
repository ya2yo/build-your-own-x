#!/usr/bin/env bash
set -euo pipefail

# Run the allocator's built-in fork-isolated tests and make failures visible to
# the shell (the C test driver prints one result per child process).
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

output_file="$(mktemp)"
trap 'rm -f "$output_file"' EXIT

# Build separately so compiler status text is not mixed with test output.
./c.sh build --bin my_malloc >/dev/null
# Disable stdio buffering so forked children cannot duplicate prior output.
stdbuf -o0 target/c/my_malloc >"$output_file"

if grep -Fq 'crashed with signal' "$output_file"; then
  printf 'myalloc test failed: allocator test crashed\n' >&2
  cat "$output_file" >&2
  exit 1
fi

expected_tests=(
  "Basic Malloc passed"
  "Request more memory Malloc passed"
  "Basic Free passed"
  "Complex passed"
)
for expected in "${expected_tests[@]}"; do
  if [[ "$(grep -Fxc "$expected" "$output_file")" -ne 1 ]]; then
    printf 'myalloc test failed: expected exactly one result: %s\n' "$expected" >&2
    cat "$output_file" >&2
    exit 1
  fi
done

if ! grep -Fq 'DONE' "$output_file"; then
  printf 'myalloc test failed: test driver did not finish\n' >&2
  cat "$output_file" >&2
  exit 1
fi

printf 'myalloc: %d allocator scenarios passed\n' "${#expected_tests[@]}"
