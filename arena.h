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
#ifndef ARENA_H_INCLUDED
#define ARENA_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_MEM_NODES (8 * sizeof(uint64_t))

size_t get_native_page_size(void);
bool page_size_is_huge(size_t page_size);
void print_page_size(size_t page_size, bool use_thp);

void *alloc_arena_mmap(size_t page_size, bool use_thp, size_t arena_size);

#endif
