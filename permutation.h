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
#ifndef PERMUTATION_H_INCLUDED
#define PERMUTATION_H_INCLUDED

#include <stdint.h>
#include <stdlib.h>

typedef uint32_t perm_t;

void gen_random_permutation(perm_t *perm, size_t nr, size_t base);
void gen_ordered_permutation(perm_t *perm, size_t nr, size_t base);
int is_a_permutation(const perm_t *perm, size_t nr_elts);

// regarding the mixer:  suppose we have a stride of 256 ... what we want
// to avoid is having the entire chase at offset 0 into the 256 byte element.
// otherwise we might favour one bank/branch/etc. of the memory system.
// similarly when we perform a parallel chase with multiple threads we don't
// want one of the many chases to favour a particular offset into the stride.
// so we have a "mixer" permutation.
//
// consider the arena as a large set of elements of size stride, then the naive
// "mixer" would use some fixed offset into each element for a particular
// (thread number, parallel chase) index.  the mixer is a function on (element
// index, thread number, parallel chase) which makes the offset into the element
// unpredictable.
// 
// the actual mixer is implemented as a large set of permutations on the
// low bits of the element number.  the details are private to the implementation.

// these are the common args required for generating all chases (and the mixer).
struct generate_chase_common_args {
  char *arena;                  // memory used for all chases
  size_t total_memory;          // size of the arena
  size_t stride;                // size of each element
  size_t tlb_locality;          // group accesses within this range in order to
                                // amortize TLB fills
  void (*gen_permutation)(perm_t *, size_t, size_t); // function for generating
                                // permutations
                                // typically gen_random_permutation
  size_t nr_mixer_indices;      // number of mixer indices
                                // typically stride/sizeof(void*)
  const perm_t *mixer;          // the mixer function itself
};

// create the mixer table
void generate_chase_mixer(struct generate_chase_common_args *args);

// create a chase for the given mixer_idx and return its first pointer
void *generate_chase(const struct generate_chase_common_args *args, size_t mixer_idx);

//============================================================================
// we want to generate a "random" pattern but we want it to be reproduceable
// across systems and c libraries... so we use this simple LCRNG (constants
// stolen from ISO C std.).
//
// we want to generate permutations on the order of 2**29 elements... (64GiB
// with stride 128:  2**36/2**7 = 2**29).
//
// this LCRNG has a period of 2**32 with this modulus... starting seed is
// basically two iterations of a seed of 1.
//
// see <http://members.cox.net/srice1/random/random4.html>

#define LCRNG_A UINT64_C(0x41c64e6d)
#define LCRNG_B UINT64_C(0x3039)

extern __thread uint64_t rng_state;

static inline void rng_init(unsigned thread_num)
{
        rng_state = (LCRNG_A + LCRNG_B)*thread_num;
}

#define RNG_MODULUS (UINT64_C(1) << 32)
static inline perm_t rng_int(perm_t limit)
{
        // much more uniform to use [0.,1.) multiply than use an integer modulus
        double res = (rng_state & (RNG_MODULUS-1)) / (RNG_MODULUS*1.0);
        rng_state = rng_state*LCRNG_A + LCRNG_B;
        return (limit + 1) * res;
}

#endif
