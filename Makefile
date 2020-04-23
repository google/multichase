# Copyright 2015 Google Inc. All Rights Reserved.
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
#
CFLAGS=-std=gnu99 -g -O2 -fomit-frame-pointer -fno-unroll-loops -Wall -Wstrict-prototypes -Wmissing-prototypes -Wshadow -Wmissing-declarations -Wnested-externs -Wpointer-arith -W -Wno-unused-parameter -Werror -pthread -Wno-tautological-compare
LDFLAGS=-g -O2 -static -pthread
LDLIBS=-lrt

ARCH := $(shell uname -m)

ifeq ($(ARCH),aarch64)
 CAP := $(shell cat /proc/cpuinfo | grep atomics | head -1)
 ifneq (,$(findstring atomics,$(CAP)))
  CFLAGS+=-march=armv8.1-a+lse
 endif
endif

EXE=multichase fairness pingpong

all: $(EXE)

clean:
	rm -f $(EXE) *.o expand.h

.c.s:
	$(CC) $(CFLAGS) -S -c $<

multichase: multichase.o permutation.o arena.o util.o

fairness: LDLIBS += -lm

expand.h: gen_expand
	./gen_expand 200 >expand.h.tmp
	mv expand.h.tmp expand.h

depend:
	makedepend -Y -- $(CFLAGS) -- *.c

# DO NOT DELETE

arena.o: arena.h
multichase.o: cpu_util.h timer.h expand.h permutation.h arena.h util.h
permutation.o: permutation.h
util.o: util.h
fairness.o: cpu_util.h expand.h timer.h
pingpong.o: cpu_util.h timer.h
