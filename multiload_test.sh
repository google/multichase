#!/bin/bash
# Copyright 2026 Google LLC
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

MULTILOAD="${1:-./multiload}"
TMPDIR_TEST=$(mktemp -d)
trap 'rm -rf "${TMPDIR_TEST}"' EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# 1. Undersized memset-nt buffers (< 128 bytes: 1 cache line + up to 63 bytes
#    of alignment padding) must fail fast with exit status 1.
rc=0
output=$(timeout 10 "${MULTILOAD}" -l memset-nt -m 120 -s 8 -n 1 -t 1 2>&1) || rc=$?
if [[ "${rc}" -ne 1 ]]; then
  fail "Expected exit 1 for -l memset-nt -m 120 -s 8, got rc=${rc}: ${output}"
fi
case "${output}" in
  *"total_memory is too small for memset-nt"*) ;;
  *) fail "Missing size error for memset-nt -m 120: ${output}" ;;
esac

# 2. Verify both aligned (-O 0, pad=0) and unaligned (-O 1, pad=63) 128-byte
#    memset-nt runs align `a` to 64 bytes and report positive bandwidth.
run_case() {
  local tag="$1"
  shift
  local case_rc=0
  timeout 20 "${MULTILOAD}" "$@" >"${TMPDIR_TEST}/${tag}.out" 2>&1 || case_rc=$?
  echo "${case_rc}" >"${TMPDIR_TEST}/${tag}.rc"
}

run_case nt_aligned   -v -v -l memset-nt -m 128 -s 8 -O 0 -n 1 -t 1 &
run_case nt_unaligned -v -v -l memset-nt -m 128 -s 8 -O 1 -n 1 -t 1 &
wait

check_case() {
  local tag="$1" expected_pad="$2"
  local case_rc line arena a
  case_rc=$(cat "${TMPDIR_TEST}/${tag}.rc")
  if [[ "${case_rc}" -ne 0 ]]; then
    fail "${tag} exited with rc=${case_rc}: $(cat "${TMPDIR_TEST}/${tag}.out")"
  fi
  if ! awk -F',' '$1 ~ /^1[[:space:]]*$/ && $9+0 > 0 {found=1} END {exit !found}' "${TMPDIR_TEST}/${tag}.out"; then
    fail "${tag} did not report positive bandwidth: $(cat "${TMPDIR_TEST}/${tag}.out")"
  fi
  line=$(awk '/load_arena=/ {print; exit}' "${TMPDIR_TEST}/${tag}.out")
  [[ -n "${line}" ]] || fail "${tag} missing load_arena debug line"
  arena=$(echo "${line}" | sed -E 's/.*load_arena=(0x[0-9a-fA-F]+).*/\1/')
  a=$(echo "${line}" | sed -E 's/.* a=(0x[0-9a-fA-F]+).*/\1/')
  if (( a % 64 != 0 )); then
    fail "${tag}: expected 64B-aligned a, got ${a} (${line})"
  fi
  if (( a - arena != expected_pad )); then
    fail "${tag}: expected pad=${expected_pad}, got $((a - arena)) (${line})"
  fi
}

check_case nt_aligned 0
check_case nt_unaligned 63

echo "PASS"
