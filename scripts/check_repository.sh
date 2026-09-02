#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "FAIL  this directory has not been initialized with git" >&2
  exit 1
fi

failures=0
fail() { printf 'FAIL  %s\n' "$1" >&2; failures=$((failures + 1)); }

for required in LICENSE README.md CONTRIBUTING.md SECURITY.md host/.env.example host/package-lock.json; do
  [ -f "$required" ] || fail "missing $required"
done

for forbidden in host/.env firmware/sdkconfig firmware/sdkconfig.old; do
  if git ls-files --error-unmatch "$forbidden" >/dev/null 2>&1; then
    fail "$forbidden must not be tracked"
  fi
done

artifact_hits=$(git ls-files | grep -E '(^|/)(node_modules|build|managed_components|backups|tmp)/' || true)
if [ -n "$artifact_hits" ]; then
  printf '%s\n' "$artifact_hits" >&2
  fail "generated artifacts or private backups are tracked"
fi

machine_hits=$(git grep -n -E '/Users/[[:alnum:]_.-]+/' -- . ':!scripts/check_repository.sh' || true)
if [ -n "$machine_hits" ]; then
  printf '%s\n' "$machine_hits" >&2
  fail "machine-specific absolute paths are tracked"
fi

secret_hits=$(git grep -n -E 'sk-[A-Za-z0-9_.-]{16,}' -- . ':!scripts/check_repository.sh' || true)
if [ -n "$secret_hits" ]; then
  printf '%s\n' "$secret_hits" | sed -E 's/(sk-)[A-Za-z0-9_.-]+/\1<redacted>/g' >&2
  fail "a value resembling an API key is tracked"
fi

for script in scripts/*.sh; do
  sh -n "$script" || fail "$script has invalid shell syntax"
done

if [ "$failures" -ne 0 ]; then
  printf '\n%d repository check(s) failed.\n' "$failures" >&2
  exit 1
fi

echo "Repository hygiene checks passed."
