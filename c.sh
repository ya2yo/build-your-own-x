#!/usr/bin/env bash
set -euo pipefail

# A small Cargo-like frontend for standalone C programs in src/.
# Usage: ./c [run|build] --bin <name> [-- program arguments]
#        ./c --test --bin <name>

command=run
bin_name=
test_mode=false
program_args=()
parsing_program_args=false

usage() {
  cat <<'EOF'
Usage:
  ./c [run|build] --bin <name> [program arguments]
  ./c [run|build] --bin <name> -- [program arguments]
  ./c --list
  ./c --test --bin <name>

Examples:
  ./c run --bin sqlite
  ./c --bin my_malloc
  ./c run --bin sqlite -- ".tables"
  ./c --test --bin sqlite

With --test, the matching test/<name>/test.sh is executed.
For my_malloc, the test directory is test/myalloc/.
Each binary name resolves to src/<name>.c and is built under target/c/.
EOF
}

while (($#)); do
  if $parsing_program_args; then
    program_args+=("$1")
    shift
    continue
  fi

  case "$1" in
    run|build)
      command="$1"
      shift
      ;;
    --test)
      test_mode=true
      shift
      ;;
    --bin)
      if (($# < 2)); then
        echo "error: --bin requires a name" >&2
        usage >&2
        exit 2
      fi
      bin_name="$2"
      shift 2
      ;;
    --list)
      printf '%s\n' "Available binaries:"
      for source in src/*.c; do
        [[ -e "$source" ]] || continue
        printf '  %s\n' "$(basename "$source" .c)"
      done
      exit 0
      ;;
    --)
      parsing_program_args=true
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      # Once a binary is selected, accept positional arguments as a
      # convenient shorthand for the program's arguments.  This keeps both
      # forms valid: `--bin sqlite -- tmp.db` and `--bin sqlite tmp.db`.
      if [[ -n "$bin_name" ]]; then
        program_args+=("$1")
        shift
      else
        echo "error: unknown argument: $1" >&2
        usage >&2
        exit 2
      fi
      ;;
  esac
done

if [[ -z "$bin_name" ]]; then
  echo "error: specify a binary with --bin <name>" >&2
  usage >&2
  exit 2
fi

if $test_mode && ((${#program_args[@]} > 0)); then
  echo "error: --test does not accept program arguments" >&2
  usage >&2
  exit 2
fi

if [[ ! "$bin_name" =~ ^[A-Za-z0-9_.-]+$ ]]; then
  echo "error: invalid binary name: $bin_name" >&2
  exit 2
fi

source="src/$bin_name.c"
output="target/c/$bin_name"
if [[ ! -f "$source" ]]; then
  echo "error: source file not found: $source" >&2
  echo "hint: use ./c --list to see available binaries" >&2
  exit 1
fi

if $test_mode; then
  case "$bin_name" in
    my_malloc) test_script="test/myalloc/test.sh" ;;
    *) test_script="test/$bin_name/test.sh" ;;
  esac
  if [[ ! -x "$test_script" ]]; then
    echo "error: executable test script not found: $test_script" >&2
    exit 1
  fi
  exec "$test_script"
fi

mkdir -p target/c
: "${CC:=gcc}"
: "${CFLAGS:=-std=c17 -Wall -Wextra -Wpedantic -g}"
: "${LDFLAGS:=}"

printf 'Compiling %s -> %s\n' "$source" "$output"
# shellcheck disable=SC2086
$CC $CFLAGS "$source" $LDFLAGS -o "$output"

if [[ "$command" == run ]]; then
  exec "$output" "${program_args[@]}"
fi
