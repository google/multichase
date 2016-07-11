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
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <alloca.h>
#include <unistd.h>
#include <sched.h>

#include "cpu_util.h"
#include "timer.h"
#include "expand.h"
#include "permutation.h"
#include "arena.h"
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
#define DEF_TOTAL_MEMORY        ((size_t)256*1024*1024)
#define DEF_STRIDE              ((size_t)256)
#define DEF_NR_SAMPLES          ((size_t)5)
#define DEF_TLB_LOCALITY        ((size_t)64*4096)
#define DEF_NR_THREADS          ((size_t)1)
#define DEF_CACHE_FLUSH         ((size_t)64*1024*1024)
#define DEF_OFFSET              ((size_t)0)

int verbosity;
int print_timestamp;
int is_weighted_mbind;
uint16_t mbind_weights[MAX_MEM_NODES];

#ifdef __i386__
#define MAX_PARALLEL (6)        // maximum number of chases in parallel
#else
#define MAX_PARALLEL (10)
#endif

// forward declare
typedef struct chase_t chase_t;

// the arguments for the chase threads
typedef union {
        char pad[AVOID_FALSE_SHARING];
        struct {
                unsigned thread_num;            // which thread is this
                unsigned count;                 // count of number of iterations
                void *cycle[MAX_PARALLEL];      // initial address for the chases
                const char *extra_args;
                int dummy;                      // useful for confusing the compiler

                const struct generate_chase_common_args *genchase_args;
                size_t nr_threads;
                const chase_t *chase;
                void *flush_arena;
                size_t cache_flush_size;
        } x;
} per_thread_t;


int always_zero;

static void chase_simple(per_thread_t *t)
{
        void *p = t->x.cycle[0];

        do {
                x200(p = *(void **)p;)
        } while (__sync_add_and_fetch(&t->x.count, 200));

        // we never actually reach here, but the compiler doesn't know that
        t->x.dummy = (uintptr_t)p;
}

// parallel chases

#define declare(i)      void *p##i = start[i];
#define cleanup(i)      tmp += (uintptr_t)p##i;
#if MAX_PARALLEL == 6
#define parallel(foo)   \
        foo(0) foo(1) foo(2) foo(3) foo(4) foo(5)
#else
#define parallel(foo)   \
        foo(0) foo(1) foo(2) foo(3) foo(4) foo(5) foo(6) foo(7) foo(8) foo(9)
#endif

#define template(n, expand, inner)                                      \
        static void chase_parallel##n(per_thread_t *t)                  \
        {                                                               \
                void **start = t->x.cycle;                              \
                parallel(declare)                                       \
                do {                                                    \
                        x##expand(inner)                                \
                } while (__sync_add_and_fetch(&t->x.count, n*expand));  \
                                                                        \
                uintptr_t tmp = 0;                                      \
                parallel(cleanup)                                       \
                t->x.dummy = tmp;                                       \
        }

#if defined(__x86_64__) || defined(__i386__)
#define D(n)    asm volatile("mov (%1),%0" : "=r" (p##n) : "r" (p##n));
#else
#define D(n)    p##n = *(void **)p##n;
#endif
template(2, 100, D(0) D(1))
template(3,  66, D(0) D(1) D(2))
template(4,  50, D(0) D(1) D(2) D(3))
template(5,  40, D(0) D(1) D(2) D(3) D(4))
template(6,  32, D(0) D(1) D(2) D(3) D(4) D(5))
#if MAX_PARALLEL > 6
template(7,  28, D(0) D(1) D(2) D(3) D(4) D(5) D(6))
template(8,  24, D(0) D(1) D(2) D(3) D(4) D(5) D(6) D(7))
template(9,  22, D(0) D(1) D(2) D(3) D(4) D(5) D(6) D(7) D(8))
template(10, 20, D(0) D(1) D(2) D(3) D(4) D(5) D(6) D(7) D(8) D(9))
#endif
#undef D
#undef parallel
#undef cleanup
#undef declare


static void chase_work(per_thread_t *t)
{
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
                x25(
                        work += (uintptr_t)p;
                        p = *(void **)p;
                        for (i = 0; i < extra_work; ++i) {
                                work ^= i;
                        }
                )
        } while (__sync_add_and_fetch(&t->x.count, 25));

        // we never actually reach here, but the compiler doesn't know that
        t->x.cycle[0] = p;
        t->x.dummy = work;
}


struct incr_struct {
        struct incr_struct *next;
        unsigned incme;
};

static void chase_incr(per_thread_t *t)
{
        struct incr_struct *p = t->x.cycle[0];

        do {
                x50(++p->incme; p = *(void **)p;)
        } while (__sync_add_and_fetch(&t->x.count, 50));

        // we never actually reach here, but the compiler doesn't know that
        t->x.cycle[0] = p;
}


#if defined(__x86_64__) || defined(__i386__)
#define chase_prefetch(type)                                                                            \
        static void chase_prefetch##type(per_thread_t *t)                                               \
        {                                                                                               \
                void *p = t->x.cycle[0];                                                                \
                                                                                                        \
                do {                                                                                    \
                        x100(asm volatile("prefetch"#type" %0"::"m"(*(void **)p)); p = *(void **)p;)    \
                } while (__sync_add_and_fetch(&t->x.count, 100));                                       \
                                                                                                        \
                /* we never actually reach here, but the compiler doesn't know that */                  \
                t->x.cycle[0] = p;                                                                      \
        }
chase_prefetch(t0)
chase_prefetch(t1)
chase_prefetch(t2)
chase_prefetch(nta)
#undef chase_prefetch
#endif

#if defined(__x86_64__)
static void chase_movdqa(per_thread_t *t)
{
        void *p = t->x.cycle[0];

        do {
                x100(
                        asm volatile(
                                "\n     movdqa (%%rax),%%xmm0"
                                "\n     movdqa 16(%%rax),%%xmm1"
                                "\n     paddq %%xmm1,%%xmm0"
                                "\n     movdqa 32(%%rax),%%xmm2"
                                "\n     paddq %%xmm2,%%xmm0"
                                "\n     movdqa 48(%%rax),%%xmm3"
                                "\n     paddq %%xmm3,%%xmm0"
                                "\n     movq %%xmm0,%%rax"
                                : "=a" (p) : "0" (p));
                )
        } while (__sync_add_and_fetch(&t->x.count, 100));
        t->x.cycle[0] = p;
}

static void chase_movntdqa(per_thread_t *t)
{
        void *p = t->x.cycle[0];

        do {
                x100(
                        asm volatile(
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
                                : "=a" (p) : "0" (p));
                )
        } while (__sync_add_and_fetch(&t->x.count, 100));
        t->x.cycle[0] = p;
}


static void chase_critword2(per_thread_t *t) {
        void *p = t->x.cycle[0];
        size_t offset = strtoul(t->x.extra_args, 0, 0);
        void *q = (char *)p + offset;

        do {
                x100(
                        asm volatile("mov (%1),%0" : "=r" (p) : "r" (p));
                        asm volatile("mov (%1),%0" : "=r" (q) : "r" (q));
                )
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
        unsigned parallelism;   // number of parallel chases (at least 1)
};
static const chase_t chases[] = {
        // the default must be first
        {
                .fn = chase_simple,
                .base_object_size = sizeof(void *),
                .name = "simple",
                .usage1 = "simple",
                .usage2 = "no frills pointer dereferencing",
                .requires_arg = 0,
                .parallelism = 1,
        },
        {
                .fn = chase_work,
                .base_object_size = sizeof(void *),
                .name = "work",
                .usage1 = "work:N",
                .usage2 = "loop simple computation N times in between derefs",
                .requires_arg = 1,
                .parallelism = 1,
        },
        {
                .fn = chase_incr,
                .base_object_size = sizeof(struct incr_struct),
                .name = "incr",
                .usage1 = "incr",
                .usage2 = "modify the cache line after each deref",
                .requires_arg = 0,
                .parallelism = 1,
        },
#if defined(__x86_64__) || defined(__i386__)
#define chase_prefetch(type)                                                    \
        {                                                                       \
                .fn = chase_prefetch##type,                                     \
                .base_object_size = sizeof(void *),                             \
                .name = #type,                                                  \
                .usage1 = #type,                                                \
                .usage2 = "perform prefetch"#type" before each deref",          \
                .requires_arg = 0,                                              \
                .parallelism = 1,                                               \
        },
chase_prefetch(t0)
chase_prefetch(t1)
chase_prefetch(t2)
chase_prefetch(nta)
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
#define PAR(n) \
        {                                                                       \
                .fn = chase_parallel##n,                                        \
                .base_object_size = sizeof(void *),                             \
                .name = "parallel" #n,                                          \
                .usage1 = "parallel" #n,                                        \
                .usage2 = "alternate "#n" non-dependent chases in each thread", \
                .parallelism = n,                                               \
        },
        PAR(2)
        PAR(3)
        PAR(4)
        PAR(5)
        PAR(6)
#if MAX_PARALLEL > 6
        PAR(7)
        PAR(8)
        PAR(9)
        PAR(10)
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


static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static size_t nr_to_startup;
static int set_thread_affinity = 1;

static void *thread_start(void *data)
{
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

        // generate chases -- using a different mixer index for every
        // thread and for every parallel chase within a thread
        unsigned parallelism = args->x.chase->parallelism;
        for (unsigned par = 0; par < parallelism; ++par) {
                args->x.cycle[par] = generate_chase(
                        args->x.genchase_args,
                        parallelism * args->x.thread_num + par);
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

        // wait and/or wake up everyone if we're all ready
        pthread_mutex_lock(&wait_mutex);
        --nr_to_startup;
        if (nr_to_startup) {
                pthread_cond_wait(&wait_cond, &wait_mutex);
        }
        else {
                pthread_cond_broadcast(&wait_cond);
        }
        pthread_mutex_unlock(&wait_mutex);

        args->x.chase->fn(data);
        return NULL;
}


static void timestamp(void) {
  if (!print_timestamp) return;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  printf("%.6f ", tv.tv_sec + tv.tv_usec/1000000.);
}


int main(int argc, char **argv)
{
        char *p;
        int c;
        size_t i;
        void *(*alloc_arena)(size_t) = alloc_arena_mmap;
        size_t nr_threads = DEF_NR_THREADS;
        size_t nr_samples = DEF_NR_SAMPLES;
        size_t cache_flush_size = DEF_CACHE_FLUSH;
        size_t offset = DEF_OFFSET;
        int print_average = 0;
        const char *extra_args = NULL;
        const char *chase_optarg = chases[0].name;
        const chase_t *chase = &chases[0];
        struct generate_chase_common_args genchase_args;

        genchase_args.total_memory = DEF_TOTAL_MEMORY;
        genchase_args.stride = DEF_STRIDE;
        genchase_args.tlb_locality = DEF_TLB_LOCALITY;
        genchase_args.gen_permutation = gen_random_permutation;

        setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

        while ((c = getopt(argc, argv, "ac:F:Hm:n:oO:S:s:T:t:vXyW:"
                          )) != -1) {
                switch (c) {
                case 'a':
                        print_average = 1;
                        break;
                case 'c':
                        chase_optarg = optarg;
                        p = strchr(optarg, ':');
                        if (p == NULL) p = optarg + strlen(optarg);
                        for (i = 0; i < sizeof(chases)/sizeof(chases[0]); ++i) {
                                if (strncmp(optarg, chases[i].name, p - optarg) == 0) {
                                        break;
                                }
                        }
                        if (i == sizeof(chases)/sizeof(chases[0])) {
                                fprintf(stderr, "not a recognized chase name: %s\n", optarg);
                                goto usage;
                        }
                        chase = &chases[i];
                        if (chase->requires_arg) {
                                if (p[0] != ':' || p[1] == 0) {
                                        fprintf(stderr, "that chase requires an argument:\n-c %s\t%s\n",
                                                chase->usage1, chase->usage2);
                                        exit(1);
                                }
                                extra_args = p+1;
                        }
                        else if (*p != 0) {
                                fprintf(stderr, "that chase does not take an argument:\n-c %s\t%s\n",
                                        chase->usage1, chase->usage2);
                                exit(1);
                        }
                        break;
                case 'F':
                        if (parse_mem_arg(optarg, &cache_flush_size)) {
                                fprintf(stderr, "cache_flush_size must be a non-negative integer (suffixed with k, m, or g)\n");
                                exit(1);
                        }
                        break;
                case 'm':
                        if (parse_mem_arg(optarg, &genchase_args.total_memory) || genchase_args.total_memory == 0) {
                                fprintf(stderr, "total_memory must be a positive integer (suffixed with k, m or g)\n");
                                exit(1);
                        }
                        break;
                case 'n':
                        nr_samples = strtoul(optarg, &p, 0);
                        if (*p) {
                                fprintf(stderr, "nr_samples must be a non-negative integer\n");
                                exit(1);
                        }
                        break;
                case 'O':
                        if (parse_mem_arg(optarg, &offset)) {
                                fprintf(stderr, "offset must be a non-negative integer (suffixed with k, m, or g)\n");
                                exit(1);
                        }
                        break;
                case 'o':
                        genchase_args.gen_permutation = gen_ordered_permutation;
                        break;
                case 's':
                        if (parse_mem_arg(optarg, &genchase_args.stride)) {
                                fprintf(stderr, "stride must be a positive integer (suffixed with k, m, or g)\n");
                                exit(1);
                        }
                        break;
                case 'T':
                        if (parse_mem_arg(optarg, &genchase_args.tlb_locality)) {
                                fprintf(stderr, "tlb locality must be a positive integer (suffixed with k, m, or g)\n");
                                exit(1);
                        }
                        break;
                case 't':
                        nr_threads = strtoul(optarg, &p, 0);
                        if (*p || nr_threads == 0) {
                                fprintf(stderr, "nr_threads must be positive integer\n");
                                exit(1);
                        }
                        break;
                case 'v':
                        ++verbosity;
                        break;
                case 'H':
                        alloc_arena = alloc_arena_shm;
                        break;
                case 'W':
                        is_weighted_mbind = 1;
                        char* tok = NULL, *saveptr = NULL;
                        tok = strtok_r(optarg, ",", &saveptr);
                        while (tok != NULL) {
                                uint16_t node_id;
                                uint16_t weight;
                                int count = sscanf(tok, "%hu:%hu", &node_id, &weight);
                                if (count != 2) {
                                        fprintf(stderr, "Expecting node_id:weight\n");
                                        exit(1);
                                }
                                if (node_id >= sizeof(mbind_weights)/sizeof(mbind_weights[0])) {
                                        fprintf(stderr, "Maximum node_id is %lu\n", sizeof(mbind_weights)/sizeof(mbind_weights[0])-1);
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
                fprintf(stderr, "-a             print average latency (default is best latency)\n");
                fprintf(stderr, "-c chase       select one of several different chases:\n");
                for (i = 0; i < sizeof(chases)/sizeof(chases[0]); ++i) {
                        fprintf(stderr, "   %-12s%s\n", chases[i].usage1, chases[i].usage2);
                }
                fprintf(stderr, "               default: %s\n", chases[0].name);
                fprintf(stderr, "-m nnnn[kmg]   total memory size (default %zu)\n", DEF_TOTAL_MEMORY);
                fprintf(stderr, "               NOTE: memory size will be rounded down to a multiple of -T option\n");
                fprintf(stderr, "-n nr_samples  nr of 0.5 second samples to use (default %zu, 0 = infinite)\n", DEF_NR_SAMPLES);
                fprintf(stderr, "-o             perform an ordered traversal (rather than random)\n");
                fprintf(stderr, "-O nnnn[kmg]   offset the entire chase by nnnn bytes\n");
                fprintf(stderr, "-s nnnn[kmg]   stride size (default %zu)\n", DEF_STRIDE);
                fprintf(stderr, "-T nnnn[kmg]   TLB locality in bytes (default %zu)\n", DEF_TLB_LOCALITY);
                fprintf(stderr, "               NOTE: TLB locality will be rounded down to a multiple of stride\n");
                fprintf(stderr, "-t nr_threads  number of threads (default %zu)\n", DEF_NR_THREADS);
                fprintf(stderr, "-H             use SHM_HUGETLB for huge page allocation (if supported)\n");
                fprintf(stderr, "-F nnnn[kmg]   amount of memory to use to flush the caches after constructing\n"
                                "               the chase and before starting the benchmark (use with nta)\n"
                                "               default: %zu\n", DEF_CACHE_FLUSH);
                fprintf(stderr, "-W mbind list  list of node:weight,... pairs for allocating memory\n"
                                "               has no effect if -H flag is specified\n"
                                "               0:10,1:90 weights it as 10%% on 0 and 90%% on 1\n");
                fprintf(stderr, "-X             do not set thread affinity\n");
                fprintf(stderr, "-y             print timestamp in front of each line\n");
                exit(1);
        }

        if (genchase_args.stride < sizeof(void *)) {
                fprintf(stderr, "stride must be at least %zu\n", sizeof(void *));
                exit(1);
        }

        // ensure some sanity in the various arguments
        if (genchase_args.tlb_locality < genchase_args.stride) {
                genchase_args.tlb_locality = genchase_args.stride;
        }
        else {
                genchase_args.tlb_locality -= genchase_args.tlb_locality % genchase_args.stride;
        }

        if (genchase_args.total_memory < genchase_args.tlb_locality) {
                if (genchase_args.total_memory < genchase_args.stride) {
                        genchase_args.total_memory = genchase_args.stride;
                }
                else {
                        genchase_args.total_memory -= genchase_args.total_memory % genchase_args.stride;
                }
                genchase_args.tlb_locality = genchase_args.total_memory;
        }
        else {
                genchase_args.total_memory -= genchase_args.total_memory % genchase_args.tlb_locality;
        }

        if (sizeof(perm_t) < sizeof(size_t)
                && ((uint64_t)genchase_args.total_memory / genchase_args.stride) != (genchase_args.total_memory / genchase_args.stride)) {
                fprintf(stderr, "too many elements required -- maximum supported is %"PRIu64"\n",
                        (UINT64_C(1) << 8*sizeof(perm_t)));
                exit(1);
        }

        genchase_args.nr_mixer_indices = genchase_args.stride / chase->base_object_size;
        if (genchase_args.nr_mixer_indices < nr_threads * chase->parallelism) {
                fprintf(stderr, "the stride is too small to interleave that many threads, need at least %zu bytes\n",
                        nr_threads * chase->parallelism * chase->base_object_size);
                exit(1);
        }

        if (verbosity > 0) {
                printf("nr_threads = %zu\n", nr_threads);
                printf("total_memory = %zu (%.1f MiB)\n", genchase_args.total_memory, genchase_args.total_memory / (1024.*1024.));
                printf("stride = %zu\n", genchase_args.stride);
                printf("tlb_locality = %zu\n", genchase_args.tlb_locality);
                printf("chase = %s\n", chase_optarg);
        }

        generate_chase_mixer(&genchase_args);

        // generate the chases by launching multiple threads
        genchase_args.arena = (char *)alloc_arena(genchase_args.total_memory + offset) + offset;
        per_thread_t *thread_data = alloc_arena_mmap(nr_threads * sizeof(per_thread_t));
        void *flush_arena = NULL;
        if (cache_flush_size) {
                flush_arena = alloc_arena_mmap(cache_flush_size);
                memset(flush_arena, 1, cache_flush_size); // ensure pages are mapped
        }

        pthread_t thread;
        nr_to_startup = nr_threads;
        for (i = 0; i < nr_threads; ++i) {
                thread_data[i].x.genchase_args = &genchase_args;
                thread_data[i].x.nr_threads = nr_threads;
                thread_data[i].x.thread_num = i;
                thread_data[i].x.extra_args = extra_args;
                thread_data[i].x.chase = chase;
                thread_data[i].x.flush_arena = flush_arena;
                thread_data[i].x.cache_flush_size = cache_flush_size;
                if (pthread_create(&thread, NULL, thread_start, &thread_data[i])) {
                        perror("pthread_create");
                        exit(1);
                }
        }

        // now wait for them all to finish generating their chases and start chasing
        pthread_mutex_lock(&wait_mutex);
        if (nr_to_startup) {
                pthread_cond_wait(&wait_cond, &wait_mutex);
        }
        pthread_mutex_unlock(&wait_mutex);

        // now start sampling their progress
        nr_samples = nr_samples + 1;        // we drop the first sample
        uint64_t *cur_samples = alloca(nr_threads * sizeof(*cur_samples));
        uint64_t last_sample_time = now_nsec();
        double best = 1./0.;
        double running_sum = 0.;
        if (verbosity > 0) printf("samples (one column per thread, one row per sample):\n");
        for (size_t sample_no = 0; nr_samples == 1 || sample_no < nr_samples; ++sample_no) {
                usleep(500000);

                uint64_t sum = 0;
                for (i = 0; i < nr_threads; ++i) {
                        cur_samples[i] = __sync_lock_test_and_set(&thread_data[i].x.count, 0);
                        sum += cur_samples[i];
                }

                uint64_t cur_sample_time = now_nsec();
                uint64_t time_delta = cur_sample_time - last_sample_time;
                last_sample_time = cur_sample_time;

                // we drop the first sample because it's fairly likely one
                // thread had some advantage initially due to still having
                // portions of the chase in a cache.
                if (sample_no == 0) continue;

                if (verbosity > 0) {
                        timestamp();
                        for (i = 0; i < nr_threads; ++i) {
                                double z = time_delta / (double)cur_samples[i];
                                printf(" %6.*f", z < 100. ? 3 : 1, z);
                        }
                }

                double t = time_delta / (double)sum;
                running_sum += t;
                if (t < best) {
                        best = t;
                }
                if (verbosity > 0) {
                        double z = t*nr_threads;
                        printf("  avg=%.*f\n", z < 100. ? 3 : 1, z);
                }
        }
        timestamp();
        double res;
        if (print_average) {
          res = running_sum * nr_threads / (nr_samples - 1);
        }
        else {
          res = best * nr_threads;
        }
        printf("%6.*f\n", res < 100. ? 3 : 1, res);

        exit(0);

        return 0;
}
