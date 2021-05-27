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
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "cpu_util.h"
#include "timer.h"

#define NR_SAMPLES (5)
#define SAMPLE_US (250000)

static size_t nr_relax = 10;
static size_t nr_tested_cores = ~0;

typedef unsigned atomic_t;

// this points to the mutex which will be pingponged back and forth
// from core to core.  it is allocated with mmap by the even thread
// so that it should be local to at least one of the two cores (and
// won't have any false sharing issues).
static atomic_t *pingpong_mutex;

// try avoid false sharing by padding out the atomic_t
typedef union {
  atomic_t x;
  char pad[AVOID_FALSE_SHARING];
} big_atomic_t __attribute__((aligned(AVOID_FALSE_SHARING)));
static big_atomic_t nr_pingpongs;

// an array we optionally modify to examine the effect of passing
// more dirty data between caches.
size_t nr_array_elts = 0;
size_t *communication_array;

//
static volatile int stop_loops;

typedef struct {
  cpu_set_t cpus;
  atomic_t me;
  atomic_t buddy;
} thread_args_t;

static void common_setup(thread_args_t *args) {
  // move to our target cpu
  if (sched_setaffinity(0, sizeof(cpu_set_t), &args->cpus)) {
    perror("sched_setaffinity");
    exit(1);
  }

  // test if we're supposed to allocate the pingpong_mutex memory
  if (args->me == 0) {
    pingpong_mutex = mmap(0, getpagesize(), PROT_READ | PROT_WRITE,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
    if (pingpong_mutex == MAP_FAILED) {
      perror("mmap");
      exit(1);
    }
    *pingpong_mutex = args->me;
  }

  // ensure both threads are ready before we leave -- so that
  // both threads have a copy of pingpong_mutex.
  static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
  static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
  static int wait_for_buddy = 1;
  pthread_mutex_lock(&wait_mutex);
  if (wait_for_buddy) {
    wait_for_buddy = 0;
    pthread_cond_wait(&wait_cond, &wait_mutex);
  } else {
    wait_for_buddy = 1;  // for next invocation
    pthread_cond_broadcast(&wait_cond);
  }
  pthread_mutex_unlock(&wait_mutex);
}

#define template(name, xchg)                                          \
  static void *name(void *data) {                                     \
    thread_args_t *args = (thread_args_t *)data;                      \
                                                                      \
    common_setup(args);                                               \
                                                                      \
    atomic_t nr = 0;                                                  \
    atomic_t me = args->me;                                           \
    atomic_t buddy = args->buddy;                                     \
    atomic_t *cache_pingpong_mutex = pingpong_mutex;                  \
    while (1) {                                                       \
      if (stop_loops) {                                               \
        pthread_exit(0);                                              \
      }                                                               \
                                                                      \
      if (xchg(cache_pingpong_mutex, me, buddy)) {                    \
        for (size_t x = 0; x < nr_array_elts; ++x) {                  \
          ++communication_array[x];                                   \
        }                                                             \
        /* don't do the atomic_add every time... it costs too much */ \
        ++nr;                                                         \
        if (nr == 10000 && me == 0) {                                 \
          __sync_fetch_and_add(&nr_pingpongs.x, 2 * nr);              \
          nr = 0;                                                     \
        }                                                             \
      }                                                               \
      for (size_t i = 0; i < nr_relax; ++i) {                         \
        cpu_relax();                                                  \
      }                                                               \
    }                                                                 \
  }

template(locked_loop, __sync_bool_compare_and_swap)

    static inline int unlocked_xchg(atomic_t *p, atomic_t old, atomic_t new) {
  if (*(volatile atomic_t *)p == old) {
    *(volatile atomic_t *)p = new;
    return 1;
  }
  return 0;
}

template(unlocked_loop, unlocked_xchg)

    static void *xadd_loop(void *data) {
  thread_args_t *args = (thread_args_t *)data;

  common_setup(args);
  uint64_t *xadder = (uint64_t *)pingpong_mutex;
  atomic_t me = args->me;
  uint64_t add_amt = (me == 0) ? 1 : (1ull << 32);
  uint32_t last_lo = 0;
  atomic_t nr = 0;

  while (1) {
    if (stop_loops) {
      pthread_exit(0);
    }

    uint64_t swap = __sync_fetch_and_add(xadder, add_amt);
    if (me == 1 && last_lo != (uint32_t)swap) {
      last_lo = swap;
      ++nr;
      if (nr == 10000) {
        __sync_fetch_and_add(&nr_pingpongs.x, 2 * nr);
        nr = 0;
      }
    }
    for (size_t i = 0; i < nr_relax; ++i) {
      cpu_relax();
    }
  }
}

int main(int argc, char **argv) {
  void *(*thread_fn)(void *data) = NULL;
  int c;
  char *p;

  while ((c = getopt(argc, argv, "c:lur:xs:")) != -1) {
    switch (c) {
      case 'l':
        if (thread_fn) goto thread_fn_error;
        thread_fn = locked_loop;
        break;
      case 'u':
        if (thread_fn) goto thread_fn_error;
        thread_fn = unlocked_loop;
        break;
      case 'x':
        if (thread_fn) goto thread_fn_error;
        thread_fn = xadd_loop;
        break;
      case 'r':
        nr_relax = strtoul(optarg, &p, 0);
        if (*p) {
          fprintf(stderr, "-r requires a numeric argument\n");
          exit(1);
        }
        break;
      case 'c':
        nr_tested_cores = strtoul(optarg, &p, 0);
        if (*p) {
          fprintf(stderr, "-c requires a numeric argument\n");
          exit(1);
        }
        break;
      case 's':
        nr_array_elts = strtoul(optarg, &p, 0);
        if (*p) {
          fprintf(stderr, "-s requires a numeric argument\n");
          exit(1);
        }
        if (posix_memalign((void **)&communication_array, 1ull << 21,
                           nr_array_elts * sizeof(*communication_array))) {
          fprintf(stderr, "posix_memalign failed\n");
          exit(1);
        }
        break;
      default:
        fprintf(stderr,
                "usage: %s [-l | -u | -x] [-r nr_relax] [-s "
                "nr_array_elts_to_dirty] [-c nr_tested_cores]\n",
                argv[0]);
        exit(1);
    }
  }
  if (thread_fn == NULL) {
  thread_fn_error:
    fprintf(stderr, "must specify exactly one of -u, -l or -x\n");
    exit(1);
  }

  setvbuf(stdout, NULL, _IONBF, BUFSIZ);

  // find the active cpus
  cpu_set_t cpus;
  if (sched_getaffinity(getpid(), sizeof(cpus), &cpus)) {
    perror("sched_getaffinity");
    exit(1);
  }

  printf(
      "avg latency to communicate a modified line from one core to another\n");
  printf("times are in ns\n\n");

  // print top row header
  const int col_width = 8;
  size_t first_cpu = ~0;
  size_t last_cpu = 0;
  printf("   ");
  for (size_t j = 0; j < CPU_SETSIZE; ++j) {
    if (CPU_ISSET(j, &cpus)) {
      if (first_cpu > j) {
        first_cpu = j;
      } else {
        printf("%*zu", col_width, j);
      }
      if (last_cpu < j) {
        last_cpu = j;
      }
    }
  }
  printf("\n");

  for (size_t i = 0, core = 0; i < last_cpu && core < nr_tested_cores; ++i) {
    if (!CPU_ISSET(i, &cpus)) {
      continue;
    }
    ++core;
    thread_args_t even;
    CPU_ZERO(&even.cpus);
    CPU_SET(i, &even.cpus);
    even.me = 0;
    even.buddy = 1;
    printf("%2zu:", i);
    for (size_t j = first_cpu + 1; j <= i; ++j) {
      if (CPU_ISSET(j, &cpus)) {
        printf("%*s", col_width, "");
      }
    }
    for (size_t j = i + 1; j <= last_cpu; ++j) {
      if (!CPU_ISSET(j, &cpus)) {
        continue;
      }

      thread_args_t odd;
      CPU_ZERO(&odd.cpus);
      CPU_SET(j, &odd.cpus);
      odd.me = 1;
      odd.buddy = 0;
      __sync_lock_test_and_set(&nr_pingpongs.x, 0);
      pthread_t odd_thread;
      if (pthread_create(&odd_thread, NULL, thread_fn, &odd)) {
        perror("pthread_create odd");
        exit(1);
      }
      pthread_t even_thread;
      if (pthread_create(&even_thread, NULL, thread_fn, &even)) {
        perror("pthread_create even");
        exit(1);
      }

      uint64_t last_stamp = now_nsec();
      double best_sample = 1. / 0.;  // infinity
      for (size_t sample_no = 0; sample_no < NR_SAMPLES; ++sample_no) {
        usleep(SAMPLE_US);
        atomic_t s = __sync_lock_test_and_set(&nr_pingpongs.x, 0);
        uint64_t time_stamp = now_nsec();
        double sample = (time_stamp - last_stamp) / (double)s;
        last_stamp = time_stamp;
        if (sample < best_sample) {
          best_sample = sample;
        }
      }
      printf("%*.1f", col_width, best_sample);

      stop_loops = 1;
      if (pthread_join(odd_thread, NULL)) {
        perror("pthread_join odd_thread");
        exit(1);
      }
      if (pthread_join(even_thread, NULL)) {
        perror("pthread_join even_thread");
        exit(1);
      }
      stop_loops = 0;

      if (munmap(pingpong_mutex, getpagesize())) {
        perror("munmap");
        exit(1);
      }
      pingpong_mutex = NULL;
    }
    printf("\n");
  }
  printf("\n");

  return 0;
}
