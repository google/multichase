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

# 1. Undersized stream buffers must fail fast with a clear error rather than
#    segfaulting on unsigned underflow or hanging on an empty loop.
expect_too_small() {
  local load="$1" mem="$2" stride="$3"
  local rc=0
  local output
  output=$(timeout 10 "${MULTILOAD}" -l "${load}" -m "${mem}" -s "${stride}" -n 1 -t 1 2>&1) || rc=$?
  if [[ "${rc}" -eq 0 ]]; then
    fail "Expected -l ${load} -m ${mem} -s ${stride} to fail, but exited 0"
  fi
  if [[ "${rc}" -ne 1 ]]; then
    fail "Expected clean exit 1 for -l ${load} -m ${mem} -s ${stride}, got rc=${rc}"
  fi
  case "${output}" in
    *"total_memory is too small for ${load}"*) ;;
    *) fail "Missing size error for ${load} -m ${mem}: ${output}" ;;
  esac
}

expect_too_small stream-sum 120 8
expect_too_small stream-copy 184 8
expect_too_small stream-triad 128 64
expect_too_small stream-triad 248 8
expect_too_small stream-triad-nontemporal-injection-delay 248 8

# 2. Run valid boundary cases in parallel (~6s wall-clock instead of ~45s):
#    - stream-triad at -O 0 must NOT shift an already-aligned load_arena by +64B
#    - each stream load at its exact minimum with worst-case -O 1 (pad = 63)
#      must align `a` to 64 bytes and complete cleanly
#    - byte-oriented libc loads must still work down to 8 bytes
run_case() {
  local tag="$1"
  shift
  local rc=0
  timeout 20 "${MULTILOAD}" "$@" >"${TMPDIR_TEST}/${tag}.out" 2>&1 || rc=$?
  echo "${rc}" >"${TMPDIR_TEST}/${tag}.rc"
}

run_case triad_aligned -v -v -l stream-triad -m 256 -s 64 -O 0 -n 1 -t 1 &
run_case sum_min       -v -v -l stream-sum   -m 128 -s 8  -O 1 -n 1 -t 1 &
run_case copy_min      -v -v -l stream-copy  -m 192 -s 8  -O 1 -n 1 -t 1 &
run_case triad_min     -v -v -l stream-triad -m 256 -s 8  -O 1 -n 1 -t 1 &
run_case triad_nt_min  -v -v -l stream-triad-nontemporal-injection-delay -m 256 -s 8 -O 1 -n 1 -t 1 &
run_case memcpy_tiny   -l memcpy-libc  -m 8 -s 8 -n 1 -t 1 &
run_case memset_tiny   -l memset-libc  -m 8 -s 8 -n 1 -t 1 &
run_case memsetz_tiny  -l memsetz-libc -m 8 -s 8 -n 1 -t 1 &
wait

for tag in triad_aligned sum_min copy_min triad_min triad_nt_min memcpy_tiny memset_tiny memsetz_tiny; do
  rc=$(cat "${TMPDIR_TEST}/${tag}.rc")
  if [[ "${rc}" -ne 0 ]]; then
    fail "${tag} exited with rc=${rc}: $(cat "${TMPDIR_TEST}/${tag}.out")"
  fi
  if ! awk -F',' '$1 ~ /^1[[:space:]]*$/ && $9+0 > 0 {found=1} END {exit !found}' "${TMPDIR_TEST}/${tag}.out"; then
    fail "${tag} did not report positive bandwidth: $(cat "${TMPDIR_TEST}/${tag}.out")"
  fi
done

check_alignment() {
  local tag="$1" expected_pad="$2"
  local line arena a
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

check_alignment triad_aligned 0
check_alignment sum_min 63
check_alignment copy_min 63
check_alignment triad_min 63
check_alignment triad_nt_min 63

echo "PASS"
