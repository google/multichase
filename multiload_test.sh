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

# 1. Undersized bandwidth buffers must fail fast with exit status 1 rather than
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

expect_too_small stream-triad 128 64
expect_too_small stream-copy 8 8
expect_too_small stream-triad 192 64
expect_too_small stream-triad 320 64
expect_too_small stream-triad-nontemporal-injection-delay 128 64
expect_too_small stream-triad-nontemporal-injection-delay 320 64

# 2. Valid minimum-size runs must complete cleanly and report positive bandwidth.
run_case() {
  local tag="$1"
  shift
  local rc=0
  timeout 20 "${MULTILOAD}" "$@" >"${TMPDIR_TEST}/${tag}.out" 2>&1 || rc=$?
  echo "${rc}" >"${TMPDIR_TEST}/${tag}.rc"
}

run_case sum_min      -l stream-sum   -m 8   -s 8  -n 1 -t 1 &
run_case copy_min     -l stream-copy  -m 16  -s 8  -n 1 -t 1 &
run_case triad_min    -l stream-triad -m 384 -s 64 -n 1 -t 1 &
run_case triad_nt_min -l stream-triad-nontemporal-injection-delay -m 384 -s 64 -n 1 -t 1 &
run_case memcpy_tiny  -l memcpy-libc  -m 8   -s 8  -n 1 -t 1 &
run_case memset_tiny  -l memset-libc  -m 8   -s 8  -n 1 -t 1 &
run_case memsetz_tiny -l memsetz-libc -m 8   -s 8  -n 1 -t 1 &
wait

for tag in sum_min copy_min triad_min triad_nt_min memcpy_tiny memset_tiny memsetz_tiny; do
  rc=$(cat "${TMPDIR_TEST}/${tag}.rc")
  if [[ "${rc}" -ne 0 ]]; then
    fail "${tag} exited with rc=${rc}: $(cat "${TMPDIR_TEST}/${tag}.out")"
  fi
  if ! awk -F',' '$1 ~ /^1[[:space:]]*$/ && $9+0 > 0 {found=1} END {exit !found}' "${TMPDIR_TEST}/${tag}.out"; then
    fail "${tag} did not report positive bandwidth: $(cat "${TMPDIR_TEST}/${tag}.out")"
  fi
done

echo "PASS"
