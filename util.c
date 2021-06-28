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
#include "util.h"

#include <stdlib.h>

int parse_mem_arg(const char *str, size_t *result) {
  size_t r;
  char *p;

  r = strtoull(str, &p, 0);
  switch (*p) {
    case 'k':
    case 'K':
      r *= 1024;
      ++p;
      break;
    case 'm':
    case 'M':
      r *= 1024 * 1024;
      ++p;
      break;
    case 'g':
    case 'G':
      r *= 1024 * 1024 * 1024;
      ++p;
      break;
  }
  if (*p) {
    return -1;
  }
  *result = r;
  return 0;
}
