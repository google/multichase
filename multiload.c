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
#define _GNU_SOURCE
#include <alloca.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "arena.h"
#include "cpu_util.h"
#include "expand.h"
#include "permutation.h"
#include "timer.h"
#include "util.h"

// The total memory, stride, and TLB locality have been chosen carefully for
// the current generation of CPUs:
//
// - at stride of 64 bytes the L2 next-line prefetch on p-m/core/core2 gives a
//   helping hand
//
// - at stride of 128 bytes the stream prefetcher on various p4 decides the
//   random accesses sometimes look like a stream and gives a helping hand.
//
// - the TLB locality could have been raised beyond 4 pages to defeat various
//   stream prefetchers, but you need to get out well past 32 pages before
//   all existing hw prefetchers are defeated, and then you start exceding the
//   TLB locality on several CPUs and incurring some TLB overhead.
//   Hence, the default has been changed from 16 pages to 64 pages.
//
#define DEF_TOTAL_MEMORY ((size_t)256 * 1024 * 1024)
#define DEF_STRIDE ((size_t)256)
#define DEF_NR_SAMPLES ((size_t)5)
#define DEF_TLB_LOCALITY ((size_t)64)
#define DEF_NR_THREADS ((size_t)1)
#define DEF_CACHE_FLUSH ((size_t)64 * 1024 * 1024)
#define DEF_OFFSET ((size_t)0)

#define LOAD_DELAY_WARMUP_uS \
  4000000  // Latency & Load thread warmup before data sampling starts
#define LOAD_DELAY_RUN_uS 2000000  // Data sampling request frequency
#define LOAD_DELAY_SAMPLE_uS \
  10000  // Data sample polling loop delay waiting for load threads to update
         // mutex variable

typedef enum { RUN_CHASE, RUN_BANDWIDTH, RUN_CHASE_LOADED } test_type_t;
static volatile uint64_t use_result_dummy = 0x0123456789abcdef;

static size_t default_page_size;
static size_t page_size;
static bool use_thp;
int verbosity;
int print_timestamp;
int is_weighted_mbind;
uint16_t mbind_weights[MAX_MEM_NODES];

#ifdef __i386__
#define MAX_PARALLEL (6)  // maximum number of chases in parallel
#else
#define MAX_PARALLEL (10)
#endif

// forward declare
typedef struct chase_t chase_t;

// the arguments for the chase threads
typedef union {
  char pad[AVOID_FALSE_SHARING];
  struct {
    unsigned thread_num;        // which thread is this
    volatile uint64_t count;    // return thread measurement - need 64 bits when
                                // passing bandwidth
    void *cycle[MAX_PARALLEL];  // initial address for the chases
    const char *extra_args;
    int dummy;  // useful for confusing the compiler

    const struct generate_chase_common_args *genchase_args;
    size_t nr_threads;
    const chase_t *chase;
    void *flush_arena;
    size_t cache_flush_size;

    test_type_t run_test_type;  // test type: chase or memory bandwidth
    const chase_t *memload;     // memory bandwidth function
    char *load_arena;           // load memory buffer used by this thread
    size_t load_total_memory;   // load size of the arena
    size_t load_offset;         // load offset of the arena
    size_t load_tlb_locality;   // group accesses within this range in order to
                                // amortize TLB fills
    volatile size_t sample_no;  // flag from main thread to tell bandwdith
                                // thread to start the next sample.
  } x;
} per_thread_t;

int always_zero;

static void chase_simple(per_thread_t *t) {
  void *p = t->x.cycle[0];

  do {
    x200(p = *(void **)p;)
  } while (__sync_add_and_fetch(&t->x.count, 200));

  // we never actually reach here, but the compiler doesn't know that
  t->x.dummy = (uintptr_t)p;
}

// parallel chases

#define declare(i) void *p##i = start[i];
#define cleanup(i) tmp += (uintptr_t)p##i;
#if MAX_PARALLEL == 6
#define parallel(foo) foo(0) foo(1) foo(2) foo(3) foo(4) foo(5)
#else
#define parallel(foo) \
  foo(0) foo(1) foo(2) foo(3) foo(4) foo(5) foo(6) foo(7) foo(8) foo(9)
#endif

#define template(n, expand, inner)                        \
  static void chase_parallel##n(per_thread_t *t) {        \
    void **start = t->x.cycle;                            \
    parallel(declare) do { x##expand(inner) }             \
    while (__sync_add_and_fetch(&t->x.count, n * expand)) \
      ;                                                   \
                                                          \
    uintptr_t tmp = 0;                                    \
    parallel(cleanup) t->x.dummy = tmp;                   \
  }

#if defined(__x86_64__) || defined(__i386__)
#define D(n) asm volatile("mov (%1),%0" : "=r"(p##n) : "r"(p##n));
#else
#define D(n) p##n = *(void **)p##n;
#endif
template(2, 100, D(0) D(1));
template(3, 66, D(0) D(1) D(2));
template(4, 50, D(0) D(1) D(2) D(3));
template(5, 40, D(0) D(1) D(2) D(3) D(4));
template(6, 32, D(0) D(1) D(2) D(3) D(4) D(5));
#if MAX_PARALLEL > 6
template(7, 28, D(0) D(1) D(2) D(3) D(4) D(5) D(6));
template(8, 24, D(0) D(1) D(2) D(3) D(4) D(5) D(6) D(7));
template(9, 22, D(0) D(1) D(2) D(3) D(4) D(5) D(6) D(7) D(8));
template(10, 20, D(0) D(1) D(2) D(3) D(4) D(5) D(6) D(7) D(8) D(9));
#endif
#undef D
#undef parallel
#undef cleanup
#undef declare

static void chase_work(per_thread_t *t) {
  void *p = t->x.cycle[0];
  size_t extra_work = strtoul(t->x.extra_args, 0, 0);
  size_t work = 0;
  size_t i;

  // the extra work is intended to be overlapped with a dereference,
  // but we don't want it to skip past the next dereference.  so
  // we fold in the value of the pointer, and launch the deref then
  // go into a loop performing extra work, hopefully while the
  // deref occurs.
  do {
    x25(work += (uintptr_t)p; p = *(void **)p;
        for (i = 0; i < extra_work; ++i) { work ^= i; })
  } while (__sync_add_and_fetch(&t->x.count, 25));

  // we never actually reach here, but the compiler doesn't know that
  t->x.cycle[0] = p;
  t->x.dummy = work;
}

struct incr_struct {
  struct incr_struct *next;
  unsigned incme;
};

static void chase_incr(per_thread_t *t) {
  struct incr_struct *p = t->x.cycle[0];

  do {
    x50(++p->incme; p = *(void **)p;)
  } while (__sync_add_and_fetch(&t->x.count, 50));

  // we never actually reach here, but the compiler doesn't know that
  t->x.cycle[0] = p;
}

#if defined(__x86_64__) || defined(__i386__)
#define chase_prefetch(type)                                               \
  static void chase_prefetch##type(per_thread_t *t) {                      \
    void *p = t->x.cycle[0];                                               \
                                                                           \
    do {                                                                   \
      x100(asm volatile("prefetch" #type " %0" ::"m"(*(void **)p));        \
           p = *(void **)p;)                                               \
    } while (__sync_add_and_fetch(&t->x.count, 100));                      \
                                                                           \
    /* we never actually reach here, but the compiler doesn't know that */ \
    t->x.cycle[0] = p;                                                     \
  }
chase_prefetch(t0);
chase_prefetch(t1);
chase_prefetch(t2);
chase_prefetch(nta);
#undef chase_prefetch
#endif

#if defined(__x86_64__)
static void chase_movdqa(per_thread_t *t) {
  void *p = t->x.cycle[0];

  do {
    x100(asm volatile("\n     movdqa (%%rax),%%xmm0"
                      "\n     movdqa 16(%%rax),%%xmm1"
                      "\n     paddq %%xmm1,%%xmm0"
                      "\n     movdqa 32(%%rax),%%xmm2"
                      "\n     paddq %%xmm2,%%xmm0"
                      "\n     movdqa 48(%%rax),%%xmm3"
                      "\n     paddq %%xmm3,%%xmm0"
                      "\n     movq %%xmm0,%%rax"
                      : "=a"(p)
                      : "0"(p));)
  } while (__sync_add_and_fetch(&t->x.count, 100));
  t->x.cycle[0] = p;
}

static void chase_movntdqa(per_thread_t *t) {
  void *p = t->x.cycle[0];

  do {
    x100(asm volatile(
#ifndef BINUTILS_HAS_MOVNTDQA
             "\n     .byte 0x66,0x0f,0x38,0x2a,0x00"
             "\n     .byte 0x66,0x0f,0x38,0x2a,0x48,0x10"
             "\n     paddq %%xmm1,%%xmm0"
             "\n     .byte 0x66,0x0f,0x38,0x2a,0x50,0x20"
             "\n     paddq %%xmm2,%%xmm0"
             "\n     .byte 0x66,0x0f,0x38,0x2a,0x58,0x30"
             "\n     paddq %%xmm3,%%xmm0"
             "\n     movq %%xmm0,%%rax"
#else
             "\n     movntdqa (%%rax),%%xmm0"
             "\n     movntdqa 16(%%rax),%%xmm1"
             "\n     paddq %%xmm1,%%xmm0"
             "\n     movntdqa 32(%%rax),%%xmm2"
             "\n     paddq %%xmm2,%%xmm0"
             "\n     movntdqa 48(%%rax),%%xmm3"
             "\n     paddq %%xmm3,%%xmm0"
             "\n     movq %%xmm0,%%rax"
#endif
             : "=a"(p)
             : "0"(p));)
  } while (__sync_add_and_fetch(&t->x.count, 100));
  t->x.cycle[0] = p;
}

static void chase_critword2(per_thread_t *t) {
  void *p = t->x.cycle[0];
  size_t offset = strtoul(t->x.extra_args, 0, 0);
  void *q = (char *)p + offset;

  do {
    x100(asm volatile("mov (%1),%0"
                      : "=r"(p)
                      : "r"(p));
         asm volatile("mov (%1),%0"
                      : "=r"(q)
                      : "r"(q));)
  } while (__sync_add_and_fetch(&t->x.count, 100));

  t->x.cycle[0] = (void *)((uintptr_t)p + (uintptr_t)q);
}
#endif

struct chase_t {
  void (*fn)(per_thread_t *t);
  size_t base_object_size;
  const char *name;
  const char *usage1;
  const char *usage2;
  int requires_arg;
  unsigned parallelism;  // number of parallel chases (at least 1)
};
static const chase_t chases[] = {
    // the default must be first
    {
        .fn = chase_simple,
        .base_object_size = sizeof(void *),
        .name = "simple    ",
        .usage1 = "simple",
        .usage2 = "no frills pointer dereferencing",
        .requires_arg = 0,
        .parallelism = 1,
    },
    {
        .fn = chase_simple,
        .base_object_size = sizeof(void *),
        .name = "chaseload",
        .usage1 = "chaseload",
        .usage2 = "runs simple chase with multiple memory bandwidth loads",
        .requires_arg = 0,
        .parallelism = 1,
    },
    {
        .fn = chase_work,
        .base_object_size = sizeof(void *),
        .name = "work     ",
        .usage1 = "work:N",
        .usage2 = "loop simple computation N times in between derefs",
        .requires_arg = 1,
        .parallelism = 1,
    },
    {
        .fn = chase_incr,
        .base_object_size = sizeof(struct incr_struct),
        .name = "incr     ",
        .usage1 = "incr",
        .usage2 = "modify the cache line after each deref",
        .requires_arg = 0,
        .parallelism = 1,
    },
#if defined(__x86_64__) || defined(__i386__)
#define chase_prefetch(type)                                        \
  {                                                                 \
    .fn = chase_prefetch##type, .base_object_size = sizeof(void *), \
    .name = #type, .usage1 = #type,                                 \
    .usage2 = "perform prefetch" #type " before each deref",        \
    .requires_arg = 0, .parallelism = 1,                            \
  }
    chase_prefetch(t0),
    chase_prefetch(t1),
    chase_prefetch(t2),
    chase_prefetch(nta),
#endif
#if defined(__x86_64__)
    {
        .fn = chase_movdqa,
        .base_object_size = 64,
        .name = "movdqa",
        .usage1 = "movdqa",
        .usage2 = "use movdqa to read from memory",
        .requires_arg = 0,
        .parallelism = 1,
    },
    {
        .fn = chase_movntdqa,
        .base_object_size = 64,
        .name = "movntdqa",
        .usage1 = "movntdqa",
        .usage2 = "use movntdqa to read from memory",
        .requires_arg = 0,
        .parallelism = 1,
    },
#endif
#define PAR(n)                                                        \
  {                                                                   \
    .fn = chase_parallel##n, .base_object_size = sizeof(void *),      \
    .name = "parallel" #n, .usage1 = "parallel" #n,                   \
    .usage2 = "alternate " #n " non-dependent chases in each thread", \
    .parallelism = n,                                                 \
  }
    PAR(2),
    PAR(3),
    PAR(4),
    PAR(5),
    PAR(6),
#if MAX_PARALLEL > 6
    PAR(7),
    PAR(8),
    PAR(9),
    PAR(10),
#endif
#undef PAR
#if defined(__x86_64__)
    {
        .fn = chase_critword2,
        .base_object_size = 64,
        .name = "critword2",
        .usage1 = "critword2:N",
        .usage2 = "a two-parallel chase which reads at X and X+N",
        .requires_arg = 1,
        .parallelism = 1,
    },
#endif
    {
        .fn = chase_simple,
        .base_object_size = 64,
        .name = "critword",
        .usage1 = "critword:N",
        .usage2 = "a non-parallel chase which reads at X and X+N",
        .requires_arg = 1,
        .parallelism = 1,
    },
};

//========================================================================================================
// Memory Bandwidth load generation
//========================================================================================================
#define LOAD_MEMORY_INIT_MIBPS                             \
  register uint64_t loops = 0;                             \
  size_t cur_sample = -1, nxt_sample = 0;                  \
  double time0, time1, timetot;                            \
  double bite_sum = 0, mibps = 0; /*mibps = MiB per sec.*/ \
  time0 = (double)now_nsec();

#define LOAD_MEMORY_SAMPLE_MIBPS                                             \
  loops++;                                                                   \
  /* Main thread will increment x.sample_no when it wants a sample. */       \
  nxt_sample = t->x.sample_no;                                               \
  /* printf("T(%d)%ld:%ld,%ld ", t->x.thread_num, cur_sample, nxt_sample,    \
   * t->x.count ); */                                                        \
  /* if ( (loops&0xF)==0xf ) printf("T(%d)%ld:%ld,%ld ", t->x.thread_num,    \
   * cur_sample, nxt_sample, t->x.count ); */                                \
  if ((cur_sample != nxt_sample) && (t->x.count == 0)) {                     \
    time1 = (double)now_nsec();                                              \
    bite_sum = loops * load_bites;                                           \
    timetot = time1 - time0;                                                 \
    mibps = (bite_sum * 1000000000.0) / (timetot * 1024 * 1024);             \
    /* printf(" T(%d)=%.0f:%ld:%ld ", t->x.thread_num, mibps, cur_sample,    \
     * nxt_sample ); */                                                      \
    /* printf(" %ld,%ld,%ld,%.0f:%.0f(%.1fMiBs)\n", cur_sample, loops,       \
     * t->x.count, bite_sum, timetot, mibps); */                             \
    /* update the MiB/s count. Main thread will read and set to 0 so we know \
     * this sample is done. */                                               \
    __sync_add_and_fetch(&t->x.count, (uint64_t)mibps);                      \
    cur_sample = nxt_sample;                                                 \
    loops = 0;                                                               \
    time0 = (double)now_nsec();                                              \
  }

//--------------------------------------------------------------------------------------------------------
static void load_memcpy_libc(per_thread_t *t) {
#define LOOP_OPS 2
  uint64_t load_loop = t->x.load_total_memory / LOOP_OPS;
  uint64_t load_bites = load_loop * LOOP_OPS;
  register char *a = (char *)t->x.load_arena;
  register char *b = a + load_loop;
  register char *tmp;

  LOAD_MEMORY_INIT_MIBPS
  do {
    tmp = a;
    a = b;
    b = tmp;
    memcpy((void *)a, (void *)b, load_loop);
    LOAD_MEMORY_SAMPLE_MIBPS
  } while (1);
#undef LOOP_OPS
}

//--------------------------------------------------------------------------------------------------------
static void load_memset_libc(per_thread_t *t) {
#define LOOP_OPS 1
  uint64_t load_bites = t->x.load_total_memory * LOOP_OPS;
  register char *a = (char *)t->x.load_arena;

  LOAD_MEMORY_INIT_MIBPS
  do {
    memset((void *)a, 0xdeadbeef, load_bites);
    LOAD_MEMORY_SAMPLE_MIBPS
  } while (1);
#undef LOOP_OPS
}

//--------------------------------------------------------------------------------------------------------
static void load_memsetz_libc(per_thread_t *t) {
#define LOOP_OPS 1
  uint64_t load_bites = t->x.load_total_memory * LOOP_OPS;
  register char *a = (char *)t->x.load_arena;

  LOAD_MEMORY_INIT_MIBPS
  do {
    memset((void *)a, 0, load_bites);
    LOAD_MEMORY_SAMPLE_MIBPS
  } while (1);
#undef LOOP_OPS
}

//--------------------------------------------------------------------------------------------------------
static void load_stream_triad(per_thread_t *t) {
#define LOOP_OPS 3
#define LOOP_ALIGN 16
  uint64_t load_loop, load_bites;
  register uint64_t N, i;
  register double *a;
  register double *b;
  register double *c;
  register double *tmp;

  load_loop =
      t->x.load_total_memory -
      (LOOP_OPS * LOOP_ALIGN);  // subract to allow aligning count/addresses
  load_loop = (load_loop / LOOP_OPS) &
              ~(LOOP_ALIGN - 1);  // divide by 3 buffers and align byte count on
                                  // LOOP_ALIGN byte multiple
  N = load_loop / sizeof(double);
  load_bites = N * sizeof(double) * LOOP_OPS;
  size_t aa = (((size_t)t->x.load_arena + LOOP_ALIGN) &
               ~(LOOP_ALIGN));  // align on 16 byte address
  a = (double *)aa;
  b = a + N;
  c = b + N;
  if (verbosity > 1) {
    printf(
        "load_arena=%p, load_total_memory=0x%lX, load_loop=0x%lX, N=0x%lX, "
        "a=%p, b=%p, c=%p\n",
        (char *)t->x.load_arena, t->x.load_total_memory, load_loop, N, a, b, c);
  }

  LOAD_MEMORY_INIT_MIBPS
  do {
    tmp = a;
    a = b;
    b = c;
    c = tmp;

    for (i = 0; i < N; ++i) {
      a[i] = b[i] + c[i];
    }
    LOAD_MEMORY_SAMPLE_MIBPS
  } while (1);
#undef LOOP_OPS
#undef LOOP_ALIGN
}

//--------------------------------------------------------------------------------------------------------
static void load_stream_copy(per_thread_t *t) {
#define LOOP_OPS 2
  uint64_t load_loop = t->x.load_total_memory / LOOP_OPS;
  register uint64_t N = load_loop / sizeof(double);
  uint64_t load_bites = N * sizeof(double) * LOOP_OPS;
  register uint64_t i;
  register double *a = (double *)t->x.load_arena;
  register double *b = a + N;
  register double *tmp;

  LOAD_MEMORY_INIT_MIBPS
  do {
    tmp = a;
    a = b;
    b = tmp;
    for (i = 0; i < N; ++i) {
      b[i] = a[i];
    }
    LOAD_MEMORY_SAMPLE_MIBPS
  } while (1);
#undef LOOP_OPS
}

//--------------------------------------------------------------------------------------------------------
static void load_stream_sum(per_thread_t *t) {
#define LOOP_OPS 1
  uint64_t load_loop = t->x.load_total_memory / LOOP_OPS;
  register uint64_t N = load_loop / sizeof(uint64_t);
  uint64_t load_bites = N * sizeof(uint64_t) * LOOP_OPS;
  register uint64_t i;
  register uint64_t *a = (uint64_t *)t->x.load_arena;
  register uint64_t s = 0;

  LOAD_MEMORY_INIT_MIBPS
  do {
    for (i = 0; i < N; ++i) {
      s += a[i];
    }
    LOAD_MEMORY_SAMPLE_MIBPS
    use_result_dummy += s;
  } while (1);
#undef LOOP_OPS
}

//--------------------------------------------------------------------------------------------------------
static const chase_t memloads[] = {
    // the default must be first
    {
        .fn = load_memcpy_libc,
        .base_object_size = sizeof(void *),
        .name = "memcpy-libc",
        .usage1 = "memcpy-libc",
        .usage2 = "1:1 rd:wr - memcpy()",
        .requires_arg = 0,
        .parallelism = 0,
    },
    {
        .fn = load_memset_libc,
        .base_object_size = sizeof(void *),
        .name = "memset-libc",
        .usage1 = "memset-libc",
        .usage2 = "0:1 rd:wr - memset() non-zero data",
        .requires_arg = 0,
        .parallelism = 0,
    },
    {
        .fn = load_memsetz_libc,
        .base_object_size = sizeof(void *),
        .name = "memsetz-libc",
        .usage1 = "memsetz-libc",
        .usage2 = "0:1 rd:wr - memset() zero data",
        .requires_arg = 0,
        .parallelism = 0,
    },
    {
        .fn = load_stream_copy,
        .base_object_size = sizeof(void *),
        .name = "stream-copy",
        .usage1 = "stream-copy",
        .usage2 = "1:1 rd:wr - lmbench stream copy ",
        .requires_arg = 0,
        .parallelism = 0,
    },
    {
        .fn = load_stream_sum,
        .base_object_size = sizeof(void *),
        .name = "stream-sum",
        .usage1 = "stream-sum",
        .usage2 = "1:0 rd:wr - lmbench stream sum ",
        .requires_arg = 0,
        .parallelism = 0,
    },
    {
        .fn = load_stream_triad,
        .base_object_size = sizeof(void *),
        .name = "stream-triad",
        .usage1 = "stream-triad",
        .usage2 = "2:1 rd:wr - lmbench stream triad a[i]=b[i]+(scalar*c[i])",
        .requires_arg = 0,
        .parallelism = 0,
    }};

static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static size_t nr_to_startup;
static int set_thread_affinity = 1;

static void *thread_start(void *data) {
  per_thread_t *args = data;

  // ensure every thread has a different RNG
  rng_init(args->x.thread_num);

  if (set_thread_affinity) {
    // find out which cpus we can run on and move us to an appropriate cpu
    cpu_set_t cpus;
    if (sched_getaffinity(0, sizeof(cpus), &cpus)) {
      perror("sched_getaffinity");
      exit(1);
    }
    int my_cpu;
    unsigned num = args->x.thread_num;
    for (my_cpu = 0; my_cpu < CPU_SETSIZE; ++my_cpu) {
      if (!CPU_ISSET(my_cpu, &cpus)) continue;
      if (num == 0) break;
      --num;
    }
    if (my_cpu == CPU_SETSIZE) {
      fprintf(stderr, "error: more threads than cpus available\n");
      exit(1);
    }
    CPU_ZERO(&cpus);
    CPU_SET(my_cpu, &cpus);
    if (sched_setaffinity(0, sizeof(cpus), &cpus)) {
      perror("sched_setaffinity");
      exit(1);
    }
  }
  if (args->x.run_test_type == RUN_CHASE) {
    // generate chases -- using a different mixer index for every
    // thread and for every parallel chase within a thread
    unsigned parallelism = args->x.chase->parallelism;
    for (unsigned par = 0; par < parallelism; ++par) {
      args->x.cycle[par] = generate_chase(
          args->x.genchase_args, parallelism * args->x.thread_num + par);
    }
    // handle critword2 chases
    if (strcmp(args->x.chase->name, "critword2") == 0) {
      size_t offset = strtoul(args->x.extra_args, 0, 0);
      char *p = args->x.cycle[0];
      char *q = p;
      do {
        char *next = *(char **)p;
        *(void **)(p + offset) = next + offset;
        p = next;
      } while (p != q);
    }
    // handle critword chases
    if (strcmp(args->x.chase->name, "critword") == 0) {
      size_t offset = strtoul(args->x.extra_args, 0, 0);
      char *p = args->x.cycle[0];
      char *q = p;
      do {
        char *next = *(char **)p;
        *(void **)(p + offset) = next;
        *(void **)p = p + offset;
        p = next;
      } while (p != q);
    }
    // now flush our caches
    if (args->x.cache_flush_size) {
      size_t nr_elts = args->x.cache_flush_size / sizeof(size_t);
      size_t *p = args->x.flush_arena;
      size_t sum = 0;
      while (nr_elts) {
        sum += *p;
        ++p;
        --nr_elts;
      }
      args->x.dummy += sum;
    }
  } else {
    if (verbosity > 2)
      printf("thread_start(%d) memload generate buffers\n", args->x.thread_num);
    // generate buffers
    args->x.load_arena = (char *)alloc_arena_mmap(
                             page_size, use_thp,
                             args->x.load_total_memory + args->x.load_offset) +
                         args->x.load_offset;
    memset(args->x.load_arena, 1,
           args->x.load_total_memory);  // ensure pages are mapped
  }

  if (verbosity > 2)
    printf("thread_start(%d) wait and/or wake up everyone\n",
           args->x.thread_num);
  // wait and/or wake up everyone if we're all ready
  pthread_mutex_lock(&wait_mutex);
  --nr_to_startup;
  if (nr_to_startup) {
    pthread_cond_wait(&wait_cond, &wait_mutex);
  } else {
    pthread_cond_broadcast(&wait_cond);
  }
  pthread_mutex_unlock(&wait_mutex);

  if (args->x.run_test_type == RUN_CHASE) {
    if (verbosity > 2) printf("thread_start: C(%d)\n", args->x.thread_num);
    args->x.chase->fn(data);
  } else {
    if (verbosity > 2) printf("thread_start: M(%d)\n", args->x.thread_num);
    args->x.memload->fn(data);
  }
  return NULL;
}

static void timestamp(void) {
  if (!print_timestamp) return;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  printf("%.6f ", tv.tv_sec + tv.tv_usec / 1000000.);
}

int main(int argc, char **argv) {
  char *p;
  int c;
  size_t i;
  size_t nr_threads = DEF_NR_THREADS;
  size_t nr_samples = DEF_NR_SAMPLES;
  size_t cache_flush_size = DEF_CACHE_FLUSH;
  size_t offset = DEF_OFFSET;
  int print_average = 0;
  const char *extra_args = NULL;
  const char *chase_optarg = chases[0].name;
  const chase_t *chase = &chases[0];
  const char *memload_optarg = memloads[0].name;
  const chase_t *memload = &memloads[0];
  test_type_t run_test_type =
      RUN_CHASE;  // RUN_CHASE, RUN_BANDWIDTH, RUN_CHASE_LOADED
  struct generate_chase_common_args genchase_args;

  default_page_size = page_size = get_native_page_size();

  genchase_args.total_memory = DEF_TOTAL_MEMORY;
  genchase_args.stride = DEF_STRIDE;
  genchase_args.tlb_locality = DEF_TLB_LOCALITY * default_page_size;
  genchase_args.gen_permutation = gen_random_permutation;

  setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

  while ((c = getopt(argc, argv, "ac:l:F:p:Hm:n:oO:S:s:T:t:vXyW:")) != -1) {
    switch (c) {
      case 'a':
        print_average = 1;
        break;
      case 'c':
        chase_optarg = optarg;
        p = strchr(optarg, ':');
        if (p == NULL) p = optarg + strlen(optarg);
        for (i = 0; i < sizeof(chases) / sizeof(chases[0]); ++i) {
          if (strncmp(optarg, chases[i].name, p - optarg) == 0) {
            break;
          }
        }
        if (i == sizeof(chases) / sizeof(chases[0])) {
          fprintf(stderr, "Error: not a recognized chase name: %s\n", optarg);
          goto usage;
        }
        chase = &chases[i];
        if (strncmp("chaseload", chases[i].name, 12) == 0) {
          run_test_type = RUN_CHASE_LOADED;
          if (verbosity > 0) {
            fprintf(stdout,
                    "Info: Loaded Latency chase selected. A -l memload can be "
                    "used to select a specific memory load\n");
          }
          break;
        }
        if (run_test_type == RUN_BANDWIDTH) {
          fprintf(stderr,
                  "Error: When using -l memload, the only valid -c selection "
                  "is chaseload (ie. loaded latency)\n");
          goto usage;
        }
        if (chase->requires_arg) {
          if (p[0] != ':' || p[1] == 0) {
            fprintf(stderr,
                    "Error: that chase requires an argument:\n-c %s\t%s\n",
                    chase->usage1, chase->usage2);
            exit(1);
          }
          extra_args = p + 1;
        } else if (*p != 0) {
          fprintf(stderr,
                  "Error: that chase does not take an argument:\n-c %s\t%s\n",
                  chase->usage1, chase->usage2);
          exit(1);
        }
        break;
      case 'F':
        if (parse_mem_arg(optarg, &cache_flush_size)) {
          fprintf(stderr,
                  "Error: cache_flush_size must be a non-negative integer "
                  "(suffixed with k, m, or g)\n");
          exit(1);
        }
        break;
      case 'p':
        if (parse_mem_arg(optarg, &page_size)) {
          fprintf(stderr,
                  "Error: page_size must be a non-negative integer (suffixed "
                  "with k, m, or g)\n");
          exit(1);
        }
        break;
      case 'H':
        use_thp = true;
        break;
      case 'l':
        memload_optarg = optarg;
        p = strchr(optarg, ':');
        if (p == NULL) p = optarg + strlen(optarg);
        for (i = 0; i < sizeof(memloads) / sizeof(memloads[0]); ++i) {
          if (strncmp(optarg, memloads[i].name, p - optarg) == 0) {
            break;
          }
        }
        if (i == sizeof(memloads) / sizeof(memloads[0])) {
          fprintf(stderr, "Error: not a recognized memload name: %s\n", optarg);
          goto usage;
        }
        memload = &memloads[i];
        if (run_test_type != RUN_CHASE_LOADED) {
          run_test_type = RUN_BANDWIDTH;
          if (verbosity > 0) {
            fprintf(stdout,
                    "Memory Bandwidth test selected. For loaded latency, -c "
                    "chaseload must also be selected\n");
          }
        }
        if (memload->requires_arg) {
          if (p[0] != ':' || p[1] == 0) {
            fprintf(stderr,
                    "Error: that memload requires an argument:\n-c %s\t%s\n",
                    memload->usage1, memload->usage2);
            exit(1);
          }
          extra_args = p + 1;
        } else if (*p != 0) {
          fprintf(stderr,
                  "Error: that memload does not take an argument:\n-c %s\t%s\n",
                  memload->usage1, memload->usage2);
          exit(1);
        }
        break;
      case 'm':
        if (parse_mem_arg(optarg, &genchase_args.total_memory) ||
            genchase_args.total_memory == 0) {
          fprintf(stderr,
                  "Error: total_memory must be a positive integer (suffixed "
                  "with k, m or g)\n");
          exit(1);
        }
        break;
      case 'n':
        nr_samples = strtoul(optarg, &p, 0);
        if (*p) {
          fprintf(stderr, "Error: nr_samples must be a non-negative integer\n");
          exit(1);
        }
        break;
      case 'O':
        if (parse_mem_arg(optarg, &offset)) {
          fprintf(stderr,
                  "Error: offset must be a non-negative integer (suffixed with "
                  "k, m, or g)\n");
          exit(1);
        }
        break;
      case 'o':
        genchase_args.gen_permutation = gen_ordered_permutation;
        break;
      case 's':
        if (parse_mem_arg(optarg, &genchase_args.stride)) {
          fprintf(stderr,
                  "Error: stride must be a positive integer (suffixed with k, "
                  "m, or g)\n");
          exit(1);
        }
        break;
      case 'T':
        if (parse_mem_arg(optarg, &genchase_args.tlb_locality)) {
          fprintf(stderr,
                  "Error: tlb locality must be a positive integer (suffixed "
                  "with k, m, or g)\n");
          exit(1);
        }
        break;
      case 't':
        nr_threads = strtoul(optarg, &p, 0);
        if (*p || nr_threads == 0) {
          fprintf(stderr, "Error: nr_threads must be positive integer\n");
          exit(1);
        }
        break;
      case 'v':
        ++verbosity;
        break;
      case 'W':
        is_weighted_mbind = 1;
        char *tok = NULL, *saveptr = NULL;
        tok = strtok_r(optarg, ",", &saveptr);
        while (tok != NULL) {
          uint16_t node_id;
          uint16_t weight;
          int count = sscanf(tok, "%hu:%hu", &node_id, &weight);
          if (count != 2) {
            fprintf(stderr, "Error: Expecting node_id:weight\n");
            exit(1);
          }
          if (node_id >= sizeof(mbind_weights) / sizeof(mbind_weights[0])) {
            fprintf(stderr, "Error: Maximum node_id is %lu\n",
                    sizeof(mbind_weights) / sizeof(mbind_weights[0]) - 1);
            exit(1);
          }
          mbind_weights[node_id] = weight;
          tok = strtok_r(NULL, ",", &saveptr);
        }
        break;
      case 'X':
        set_thread_affinity = 0;
        break;
      case 'y':
        print_timestamp = 1;
        break;
      default:
        goto usage;
    }
  }

  if (argc - optind != 0) {
  usage:
    fprintf(stderr, "usage: %s [options]\n", argv[0]);
    fprintf(stderr,
            "This program can run either read latency, memory bandwidth, or "
            "loaded-latency:\n");
    fprintf(stderr,
            "    Latency only;   -c MUST NOT be chaseload. -l memload MUST NOT "
            "be used\n");
    fprintf(stderr,
            "    Bandwidth only: -c MUST NOT be used.      -l memload MUST be "
            "used\n");
    fprintf(stderr,
            "    Loaded-latency: -c MUST be chaseload,     -l memload MUST be "
            "used\n");
    fprintf(stderr,
            "-a       print average latency (default is best latency)\n");
    fprintf(stderr, "-c chase       select one of several different chases:\n");
    for (i = 0; i < sizeof(chases) / sizeof(chases[0]); ++i) {
      fprintf(stderr, "   %-12s%s\n", chases[i].usage1, chases[i].usage2);
    }
    fprintf(stderr, "         default: %s\n", chases[0].name);
    fprintf(stderr,
            "-l memload     select one of several different memloads:\n");
    for (i = 0; i < sizeof(memloads) / sizeof(memloads[0]); ++i) {
      fprintf(stderr, "   %-12s%s\n", memloads[i].usage1, memloads[i].usage2);
    }
    fprintf(stderr, "         default: %s\n", memloads[0].name);
    fprintf(stderr,
            "-F nnnn[kmg]   amount of memory to use to flush the caches after "
            "constructing\n"
            "         the chase/memload and before starting the benchmark (use "
            "with nta)\n"
            "         default: %zu\n",
            DEF_CACHE_FLUSH);
    fprintf(stderr, "-p nnnn[kmg]   backing page size to use (default %zu)\n",
            default_page_size);
    fprintf(
        stderr,
        "-H       use transparent hugepages (leave page size at default)\n");
    fprintf(stderr, "-m nnnn[kmg]   total memory size (default %zu)\n",
            DEF_TOTAL_MEMORY);
    fprintf(stderr,
            "         NOTE: memory size will be rounded down to a multiple of "
            "-T option\n");
    fprintf(stderr,
            "-n nr_samples  nr of 0.5 second samples to use (default %zu, 0 = "
            "infinite)\n",
            DEF_NR_SAMPLES);
    fprintf(stderr,
            "-o       perform an ordered traversal (rather than random)\n");
    fprintf(
        stderr,
        "-O nnnn[kmg]   offset the entire chase by nnnn bytes (default %zu)\n",
        DEF_OFFSET);
    fprintf(stderr, "-s nnnn[kmg]   stride size (default %zu)\n", DEF_STRIDE);
    fprintf(stderr, "-T nnnn[kmg]   TLB locality in bytes (default %zu)\n",
            DEF_TLB_LOCALITY * default_page_size);
    fprintf(stderr,
            "         NOTE: TLB locality will be rounded down to a multiple of "
            "stride\n");
    fprintf(stderr, "-t nr_threads  number of threads (default %zu)\n",
            DEF_NR_THREADS);
    fprintf(stderr, "-v       verbose output (default %u)\n", verbosity);
    fprintf(
        stderr,
        "-W mbind list  list of node:weight,... pairs for allocating memory\n"
        "         has no effect if -H flag is specified\n"
        "         0:10,1:90 weights it as 10%% on 0 and 90%% on 1\n");
    fprintf(stderr, "-X       do not set thread affinity (default %u)\n",
            set_thread_affinity);
    fprintf(stderr,
            "-y       print timestamp in front of each line (default %u)\n",
            print_timestamp);
    exit(1);
  }

  if (genchase_args.stride < sizeof(void *)) {
    fprintf(stderr, "stride must be at least %zu\n", sizeof(void *));
    exit(1);
  }

  // ensure some sanity in the various arguments
  if (genchase_args.tlb_locality < genchase_args.stride) {
    genchase_args.tlb_locality = genchase_args.stride;
  } else {
    genchase_args.tlb_locality -=
        genchase_args.tlb_locality % genchase_args.stride;
  }

  if (genchase_args.total_memory < genchase_args.tlb_locality) {
    if (genchase_args.total_memory < genchase_args.stride) {
      genchase_args.total_memory = genchase_args.stride;
    } else {
      genchase_args.total_memory -=
          genchase_args.total_memory % genchase_args.stride;
    }
    genchase_args.tlb_locality = genchase_args.total_memory;
  } else {
    genchase_args.total_memory -=
        genchase_args.total_memory % genchase_args.tlb_locality;
  }

  if (sizeof(perm_t) < sizeof(size_t) &&
      ((uint64_t)genchase_args.total_memory / genchase_args.stride) !=
          (genchase_args.total_memory / genchase_args.stride)) {
    fprintf(stderr,
            "too many elements required -- maximum supported is %" PRIu64 "\n",
            (UINT64_C(1) << 8 * sizeof(perm_t)));
    exit(1);
  }

  genchase_args.nr_mixer_indices =
      genchase_args.stride / chase->base_object_size;
  if ((run_test_type == RUN_CHASE) &&
      (genchase_args.nr_mixer_indices < nr_threads * chase->parallelism)) {
    fprintf(stderr,
            "the stride is too small to interleave that many threads, need at "
            "least %zu bytes\n",
            nr_threads * chase->parallelism * chase->base_object_size);
    exit(1);
  }

  if (verbosity > 0) {
    printf("nr_threads = %zu\n", nr_threads);
    print_page_size(page_size, use_thp);
    printf("total_memory = %zu (%.1f MiB)\n", genchase_args.total_memory,
           genchase_args.total_memory / (1024. * 1024.));
    printf("stride = %zu\n", genchase_args.stride);
    printf("tlb_locality = %zu\n", genchase_args.tlb_locality);
    printf("chase = %s\n", chase_optarg);
    printf("memload = %s\n", memload_optarg);
    if (run_test_type == RUN_CHASE) printf("run_test_type = RUN_CHASE\n");
    if (run_test_type == RUN_BANDWIDTH)
      printf("run_test_type = RUN_BANDWIDTH\n");
    if (run_test_type == RUN_CHASE_LOADED)
      printf("run_test_type = RUN_CHASE_LOADED\n");
  }

  rng_init(1);

  if (run_test_type != RUN_BANDWIDTH) {
    generate_chase_mixer(&genchase_args);

    // generate the chases by launching multiple threads
    if (verbosity > 2) printf("allocate genchase_args.arena\n");
    genchase_args.arena =
        (char *)alloc_arena_mmap(page_size, use_thp,
                                 genchase_args.total_memory + offset) +
        offset;
  }
  per_thread_t *thread_data = alloc_arena_mmap(
      default_page_size, false, nr_threads * sizeof(per_thread_t));
  void *flush_arena = NULL;
  if (verbosity > 2) printf("allocate cache flush\n");
  if (cache_flush_size) {
    flush_arena = alloc_arena_mmap(default_page_size, false, cache_flush_size);
    memset(flush_arena, 1, cache_flush_size);  // ensure pages are mapped
  }

  pthread_t thread;
  size_t nr_chase_threads = 0, nr_load_threads = 0;
  nr_to_startup = nr_threads;
  for (i = 0; i < nr_threads; ++i) {
    thread_data[i].x.genchase_args = &genchase_args;
    thread_data[i].x.nr_threads = nr_threads;
    thread_data[i].x.thread_num = i;
    thread_data[i].x.extra_args = extra_args;
    thread_data[i].x.chase = chase;
    thread_data[i].x.flush_arena = flush_arena;
    thread_data[i].x.cache_flush_size = cache_flush_size;
    thread_data[i].x.memload = memload;
    thread_data[i].x.load_arena = NULL;  // memory buffer used by this thread
    thread_data[i].x.load_total_memory =
        genchase_args.total_memory;         // size of the arena
    thread_data[i].x.load_offset = offset;  // memory buffer offset

    if (run_test_type == RUN_CHASE_LOADED) {
      if (i == 0) {
        thread_data[i].x.run_test_type = RUN_CHASE;
        nr_chase_threads++;
        if (verbosity > 2) printf("main: Starting C[%ld]\n", i);
        if (pthread_create(&thread, NULL, thread_start, &thread_data[i])) {
          perror("pthread_create");
          exit(1);
        }
      } else {
        thread_data[i].x.run_test_type = RUN_BANDWIDTH;
        nr_load_threads++;
        if (verbosity > 2) printf("main: Starting M[%ld]\n", i);
        if (pthread_create(&thread, NULL, thread_start, &thread_data[i])) {
          perror("pthread_create");
          exit(1);
        }
      }

    } else if (run_test_type == RUN_CHASE) {
      thread_data[i].x.run_test_type = RUN_CHASE;
      nr_chase_threads++;
      if (verbosity > 2) printf("main: Starting C[%ld]\n", i);
      if (pthread_create(&thread, NULL, thread_start, &thread_data[i])) {
        perror("pthread_create");
        exit(1);
      }
    } else {
      nr_load_threads++;
      thread_data[i].x.run_test_type = RUN_BANDWIDTH;
      if (verbosity > 2) printf("main: Starting M[%ld]\n", i);
      if (pthread_create(&thread, NULL, thread_start, &thread_data[i])) {
        perror("pthread_create");
        exit(1);
      }
    }
  }

  // now wait for them all to finish generating their chases/memloads and start
  // testing
  if (verbosity > 2) printf("main: waiting for threads to initialize\n");
  pthread_mutex_lock(&wait_mutex);
  if (nr_to_startup) {
    pthread_cond_wait(&wait_cond, &wait_mutex);
  }
  pthread_mutex_unlock(&wait_mutex);
  usleep(LOAD_DELAY_WARMUP_uS);  // Give OS scheduler thread migrations time to
                                 // settle down.

  if (verbosity > 2) printf("main: start sampling thread progress\n");
  // now start sampling their progress
  nr_samples = nr_samples + 1;  // we drop the first sample
  double *cur_samples = alloca(nr_threads * sizeof(*cur_samples));
  uint64_t last_sample_time, cur_sample_time;
  double chase_min = 1. / 0., chase_max = 0.;
  double chase_running_sum = 0., load_running_sum = 0.,
         chase_running_geosum = 0.;
  double load_max_mibps = 0, load_min_mibps = 1. / 0.;
  double chase_thd_sum = 0, load_thd_sum = 0;
  uint64_t time_delta = 0;
  int ready;

  last_sample_time = now_nsec();
  for (size_t sample_no = 0; nr_samples == 1 || sample_no < nr_samples;
       ++sample_no) {
    if (verbosity > 0) printf("main: sample_no=%ld ", sample_no);
    usleep(LOAD_DELAY_RUN_uS);
    // Request threads to update their sample
    for (i = 0; i < nr_threads; ++i) {
      thread_data[i].x.sample_no = sample_no;
    }

    chase_thd_sum = 0.;
    load_thd_sum = 0.;
    usleep(LOAD_DELAY_SAMPLE_uS);  // Give load threads time to update sample
                                   // count. Chase threads are always updating
    for (i = 0; i < nr_threads; ++i) {
      if (verbosity > 2) printf("-");
      ready = 0;
      while (ready == 0) {
        cur_samples[i] =
            (double)__sync_lock_test_and_set(&thread_data[i].x.count, 0);
        if (cur_samples[i] != 0) {
          // Chase threads start at thread 0 and should always be ready,
          // therefore we read chase timestamp as soon as finished reading the
          // last chase thread Load threads return pre-calculated MiB/s so don't
          // use this timer
          if ((i + 1) == nr_chase_threads) {
            cur_sample_time = now_nsec();
            time_delta = cur_sample_time - last_sample_time;
            last_sample_time = cur_sample_time;
          }
          ready = 1;
        } else {
          if (verbosity > 2) printf("*");
          usleep(LOAD_DELAY_SAMPLE_uS);
        }
      }
    }

    for (i = 0; i < nr_threads; ++i) {
      // printf("main: thread[%d], run_mode=%i\n", t->x.thread_num,
      // t->x.run_test_type);
      if (thread_data[i].x.run_test_type == RUN_CHASE) {
        chase_thd_sum += (double)cur_samples[i];
        if (verbosity > 1) {
          double z = time_delta / (double)cur_samples[i];
          double mibps = sizeof(void *) / (z / 1000000000.0) / (1024 * 1024);
          printf(" MC(%ld)%.3f, %6.1f(ns), %.3f(MiB/s)", i, cur_samples[i], z,
                 mibps);
        }
      } else {
        load_thd_sum += (double)cur_samples[i];
        if (verbosity > 1) {
          printf(" ML(%ld)%.0f(MiB/s)", i, cur_samples[i]);
        }
      }
    }

    // we drop the first sample because it's fairly likely one
    // thread had some advantage initially due to still having
    // portions of the chase in a cache.
    if (sample_no == 0) {
      if (verbosity > 0) printf("\n");
      continue;
    }

    // Calcuate chase overall thread stats.
    if (chase_thd_sum != 0) {
      double t = time_delta / (double)chase_thd_sum;
      chase_running_sum += t;
      chase_running_geosum += log(t);
      if (t < chase_min) chase_min = t;
      if (t > chase_max) chase_max = t;
      if (verbosity > 0) {
        double z = t * nr_chase_threads;
        printf(" avg=%.1f(ns)\n", z);
      }
    }

    // Calculate memory load overall thread stats
    if (load_thd_sum != 0) {
      if (load_thd_sum > load_max_mibps) load_max_mibps = load_thd_sum;
      if (load_thd_sum < load_min_mibps) load_min_mibps = load_thd_sum;
      load_running_sum += load_thd_sum;
      if (verbosity > 0) {
        printf(" main: threads=%ld, Total(MiB/s)=%.*f, PerThread=%.f\n",
               nr_load_threads, load_thd_sum < 100. ? 3 : 1, load_thd_sum,
               load_thd_sum / nr_load_threads);
      }
    }
  }

  // printf("sample_sum=%.f\n", sample_sum);
  // printf("main: float=%li, void*=%li, size_t=%li, uint64_t=%li, double=%li,
  // long=%li, int=%li\n",
  //       sizeof(float), sizeof(void*), sizeof(size_t), sizeof(uint64_t),
  //       sizeof(double), sizeof(long), sizeof(int) );
  double ChasNS = 0, ChasDEV = 0, ChasBEST = 0, ChasWORST = 0, ChasAVG = 0,
         ChasMibs = 0, ChasGEO = 0;
  double LdAvgMibs = 0, LdMibsDEV = 0;

  if (nr_chase_threads != 0) {
    ChasAVG = chase_running_sum * nr_chase_threads / (nr_samples - 1);
    ChasGEO =
        nr_chase_threads * exp(chase_running_geosum / ((double)nr_samples - 1));
    ChasBEST = chase_min * nr_chase_threads;
    ChasWORST = chase_max * nr_chase_threads;
    ChasDEV = ((ChasWORST - ChasBEST) / ChasAVG);
    if (verbosity > 0) {
      printf(
          "ChasAVG=%-8f, ChasGEO=%-8f, ChasBEST=%-8f, ChasWORST=%-8f, "
          "ChasDEV=%-8.3f\n",
          ChasAVG, ChasGEO, ChasBEST, ChasWORST, ChasDEV);
    }
    // if (print_average) ChasNS = ChasAVG;
    if (print_average)
      ChasNS = ChasGEO;
    else
      ChasNS = ChasBEST;
    ChasMibs = nr_chase_threads *
               (sizeof(void *) / (ChasNS / 1000000000.0) / (1024 * 1024));
  }

  if (nr_load_threads != 0) {
    LdAvgMibs = load_running_sum / (nr_samples - 1);
    LdMibsDEV = ((load_max_mibps - load_min_mibps) / LdAvgMibs);
    if (verbosity > 0) {
      printf(
          "LdAvgMibs=%-8f, LdMaxMibs=%-8f, LdMinMibs=%-8f, LdDevMibs=%-8.3f\n",
          LdAvgMibs, load_max_mibps, load_min_mibps, LdMibsDEV);
    }
  }

  const char *not_used = "--------";
  printf(
      "Samples\t, Byte/thd\t, ChaseThds\t, ChaseNS\t, ChaseMibs\t, "
      "ChDeviate\t, LoadThds\t, LdMaxMibs\t, LdAvgMibs\t, LdDeviate\t, "
      "ChaseArg\t, MemLdArg\n");
  printf(
      "%-6ld\t, %-11ld\t, %-8ld\t, %-8.3f\t, %-8.f\t, %-8.3f\t, %-8.f\t, "
      "%-8.f\t, %-8.f\t, %-8.3f",
      nr_samples - 1, thread_data[0].x.load_total_memory, nr_chase_threads,
      ChasNS, ChasMibs, ChasDEV, (double)nr_load_threads, load_max_mibps,
      LdAvgMibs, LdMibsDEV);
  switch (run_test_type) {
    case RUN_CHASE_LOADED:
      printf("\t, %s\t, %s\n", chase_optarg, memload_optarg);
      break;
    case RUN_BANDWIDTH:
      printf("\t, %s\t, %s\n", not_used, memload_optarg);
      break;
    default:
      printf("\t, %s\t, %s\n", chase_optarg, not_used);
  }

  timestamp();
  exit(0);
}
