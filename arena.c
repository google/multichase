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
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "arena.h"

extern int verbosity;

void *alloc_arena_mmap(size_t arena_size)
{
	void *arena;
        int pagemask = getpagesize() - 1;

        arena_size = (arena_size + pagemask) & ~pagemask;
	arena = mmap(0, arena_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
	if (arena == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return arena;
}


#ifdef SHM_HUGETLB
void *alloc_arena_shm(size_t arena_size)
{
	FILE *fh;
	char buf[512];
	size_t huge_page_size;
	char *p;
	int shmid;
	void *arena;

	// find Hugepagesize in /proc/meminfo
	if ((fh = fopen("/proc/meminfo", "r")) == NULL) {
		perror("open(/proc/meminfo)");
		exit(1);
	}
	for (;;) {
		if (fgets(buf, sizeof(buf)-1, fh) == NULL) {
			fprintf(stderr, "didn't find Hugepagesize in /proc/meminfo");
			exit(1);
		}
		buf[sizeof(buf)-1] = '\0';
		if (strncmp(buf, "Hugepagesize:", 13) == 0) break;
	}
	p = strchr(buf, ':') + 1;
	huge_page_size = strtoul(p, 0, 0) * 1024;
	fclose(fh);

	// round the size up to multiple of huge_page_size
	arena_size = (arena_size + huge_page_size - 1) & ~(huge_page_size - 1);

        if (verbosity > 1) {
                printf("attempting to shmget %zu bytes\n", arena_size);
        }

	shmid = shmget(IPC_PRIVATE, arena_size, IPC_CREAT|IPC_EXCL|SHM_HUGETLB|0600);
	if (shmid == -1) {
		perror("shmget");
		exit(1);
	}

	arena = shmat(shmid, NULL, 0);
	if (arena == (void *)-1) {
		perror("shmat");
		exit(1);
	}

	if (shmctl(shmid, IPC_RMID, 0) == -1) {
		perror("shmctl warning");
	}

	return arena;
}
#else
void *alloc_arena_shm(size_t arena_size)
{
        return alloc_arena_mmap(arena_size);
}
#endif
