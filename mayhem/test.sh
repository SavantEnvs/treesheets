#!/usr/bin/env bash
# mayhem/test.sh — TreeSheets ships no test suite, so this is a known-answer oracle over the app's
# own loaders/importers, its Lobster script API and its plain-text exporter: mayhem/build/ts_oracle (built by build.sh
# with normal flags) starts the real app, opens a file through System::LoadDB / System::Import or runs a script\n# through TreeSheetsScriptImpl::ScriptRun,
# and prints the document's text export (or "ERROR: <message>"). Each case asserts the exact output.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${SRC:=/mayhem}"
cd "$SRC"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

O="$SRC/mayhem/build/ts_oracle"
D="$SRC/mayhem/test/data"
if [ ! -x "$O" ]; then
  echo "test.sh: $O missing (mayhem/build.sh builds it)" >&2
  check paste-tsv "${nl}  one${nl}  two${nl}  ${nl}  three${nl}  four${nl}  ${nl}  ${nl}  ${nl}  " paste "$D/paste.tsv"
emit_ctrf ts-oracle 0 1
  exit 1
fi
# The oracle writes TreeSheets.ini (portable mode) into its cwd.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/ts-test.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

passed=0 failed=0
# check <name> <expected output> <mode> <file>
check() {
  local name="$1" expected="$2" got
  got="$(cd "$WORK" && "$O" "$3" "$4" 2>/dev/null | tr -d '\r')"
  if [ "$got" == "$expected" ]; then
    echo "PASS $name"; passed=$((passed + 1))
  else
    echo "FAIL $name"; diff <(printf '%s\n' "$expected") <(printf '%s\n' "$got") | head -20
    failed=$((failed + 1))
  fi
}
nl=$'\n'
check csv-import "${nl}  name${nl}  qty${nl}  widget${nl}  2${nl}  gadget${nl}  7" csv "$D/small.csv"
check indent-import "${nl}  fruit${nl}    apple${nl}    pear${nl}  veg${nl}    leek" indent "$D/outline.txt"
check xml-import "${nl}  alpha${nl}  beta${nl}  gamma${nl}  delta" xml "$D/grid.xml"
check reject-non-cts "ERROR: Not a TreeSheets file." load "$D/notts.cts"
check reject-truncated "ERROR: File corrupted!" load "$D/truncated.cts"
check load-sales "$(cat "$D/sales.expected")" load "$SRC/TS/examples/sales.cts"
check load-simple-graph "$(cat "$D/simple_graph.expected")" load "$SRC/TS/examples/simple_graph.cts"
check script-edit "${nl}  hello${nl}  ${nl}  ${nl}  ${nl}  =1+2${nl}  ${nl}  ${nl}  ${nl}  0" script "$D/edit.lobster"
check script-bad-grid-size "ERROR: $(cat "$D/bad_grid.expected")" script "$D/bad_grid.lobster"
check paste-tsv "${nl}  one${nl}  two${nl}  ${nl}  three${nl}  four${nl}  ${nl}  ${nl}  ${nl}  " paste "$D/paste.tsv"
emit_ctrf ts-oracle "$passed" "$failed"
