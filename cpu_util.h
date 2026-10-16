/* Copyright 2015 Google Inc. All Rights Reserved.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef PORT_H_INCLUDED
#define PORT_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

// We assume 1024 bytes is good enough alignment to avoid false sharing on all
// architectures.
#define AVOID_FALSE_SHARING (1024)
#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif
#ifndef SWEEP_MAX
#define SWEEP_MAX 256
#endif
#ifndef SWEEP_SPACER
#define SWEEP_SPACER (CACHELINE_SIZE - sizeof(unsigned))
#endif

#if defined(__x86_64__) || defined(__i386__)
static inline void cpu_relax(void) { asm volatile("rep; nop"); }
#elif defined __powerpc__
static inline void cpu_relax(void) {
  // HMT_low()
  asm volatile("or 1,1,1");
  // HMT_medium()
  asm volatile("or 2,2,2");
  // barrier()
  asm volatile("" : : : "memory");
}
#elif defined(__aarch64__)
#define cpu_relax() asm volatile("yield" ::: "memory")
#else
#warning "no cpu_relax for your cpu"
#define cpu_relax() \
  do {              \
  } while (0)
#endif

// clean_invalidate_range() writes back and invalidates [buf, buf + len) to
// the point of coherency, so a buffer that was just initialized is clean in
// every cache level. A read-only test that starts on dirty lines can generate
// write traffic on every pass: on some system level caches a read hit on a
// dirty line hands the line back dirty, and the core later writes it back.
// Only implemented on aarch64, where DC CIVAC is usable from user space on
// Linux. Elsewhere it is a no-op and HAVE_CLEAN_INVALIDATE_RANGE is 0.
#if defined(__aarch64__)
#define HAVE_CLEAN_INVALIDATE_RANGE 1
static inline void clean_invalidate_range(const void *buf, size_t len) {
  // Rounding buf down below would otherwise touch one line when len is 0.
  if (len == 0) return;
  uint64_t ctr;
  asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
  // CTR_EL0.DminLine is log2 of the smallest data cache line in words.
  const uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 0xf);
  uintptr_t p = (uintptr_t)buf & ~(line - 1);
  const uintptr_t end = (uintptr_t)buf + len;
  for (; p < end; p += line) {
    asm volatile("dc civac, %0" : : "r"(p) : "memory");
  }
  asm volatile("dsb ish" : : : "memory");
}
#else
#define HAVE_CLEAN_INVALIDATE_RANGE 0
static inline void clean_invalidate_range(const void *buf, size_t len) {
  (void)buf;
  (void)len;
}
#endif

#endif
