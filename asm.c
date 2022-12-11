#include "asm.h"

int cycle_len(void *p) {
  int count = 0;
  char *next = (char *)p;
  do {
    ++count;
    next = *(char **)next;
  } while (next != p);
  return count;
}

#if defined(__aarch64__)

uint64_t rbits(uint64_t val, int rmask) { return val & ((1ULL << rmask) - 1); }

uint64_t rmask_lsh(uint64_t val, int rmask, int left_shift) {
  return (val & ((1ULL << rmask) - 1)) << left_shift;
}

void arm_emit_adrp(char *p, int rd, int val21) {
  *(int *)p = (9 << 28) | rmask_lsh(val21, 2, 29) |
              rmask_lsh(val21 >> 2, 19, 5) | rmask_lsh(rd, 5, 0);
}

void arm_emit_adds_imm(char *p, int rd, int rn, int imm12) {
  *(int *)p = (0b1011000100 << 22) | rmask_lsh(imm12, 12, 10) |
              rmask_lsh(rn, 5, 5) | rmask_lsh(rd, 5, 0);
}

void arm_emit_br(char *p, int rs) {
  *(int *)p = 0b11010110000111110000000000000000 | rmask_lsh(rs, 5, 5);
}

void arm_emit_movk(char *p, int rd, int imm16, int shift16) {
  *(int *)p = (0b111100101 << 23) | rmask_lsh(shift16, 16, 21) |
              rmask_lsh(imm16, 16, 5) | rmask_lsh(rd, 5, 0);
}

void arm_emit_movz(char *p, int rd, int imm16, int shift16) {
  *(int *)p = (0b110100101 << 23) | rmask_lsh(shift16, 16, 21) |
              rmask_lsh(imm16, 16, 5) | rmask_lsh(rd, 5, 0);
}

void arm_emit_ret(char *p) { *(int *)p = 0xd65f03c0; }

// Convert a pointer chase to a branch chase, returning after chunk_size
// branches with a function pointer to the next branch in the chase.
int convert_pointers_to_branches(void *head, int chunk_size) {
  int remain = cycle_len(head);
  chunk_size = (remain < chunk_size)
                   ? remain
                   : remain / (1 << lround(log2(1.0 * remain / chunk_size)));
  int base_chunk_size = chunk_size;
  int chunks_remaining = remain / chunk_size;
  int chunk_count = 0;
  // printf("convert %d pointers, use chunk_size %d\n", remain, chunk_size);
  const int ptr_reg = 0;  // Address of next branch and return value.
  char *p = (char *)head;
  do {
    if (!chunk_count) chunk_count = remain / chunks_remaining;
    char *next = *((char **)p);
    const int br_code_len = 16;
    for (int i = 8; i < br_code_len; i++) {
      if (p[i]) {
        fprintf(stderr, "not enough space to convert a pointer to branches");
        exit(1);
      }
    }
    // VA is 48 bits max.
    uint64_t shift00 = rbits((intptr_t)next, 16);
    uint64_t shift16 = rbits((intptr_t)next >> 16, 16);
    uint64_t shift32 = rbits((intptr_t)next >> 32, 16);
    arm_emit_movz(p, ptr_reg, shift00, 0);
    arm_emit_movk(p + 4, ptr_reg, shift16, 1);
    arm_emit_movk(p + 8, ptr_reg, shift32, 2);
    // At end of branch chunk, return with a pointer to the next chunk.
    --remain;
    if (--chunk_count == 0) {
      arm_emit_ret(p + 12);
      --chunks_remaining;
    } else {
      arm_emit_br(p + 12, ptr_reg);
    }
    p = next;
  } while (p != head);
  if (remain != 0 || chunks_remaining > 0) {
    fprintf(stderr, "internal error\n");
    exit(1);
  }
  return base_chunk_size;
}

#elif defined(__x86_64__)

int x64_emit_mov_imm64_rax(char *p, uint64_t imm64) {
  *p++ = 0x48;  // movabs
  *p++ = 0xb8;
  for (int i = 0; i < 8; i++) *p++ = imm64 >> (8 * i);  // immediate (8 bytes)
  return 10;
}

int x64_emit_jmp_to_rax(char *p) {
  *p++ = 0xff;  // jmp *rax
  *p++ = 0xe0;
  return 2;
}

int x64_emit_ret(char *p) {
  *p++ = 0xc3;
  return 1;
}

// Convert a pointer chase to a branch chase, returning after chunk_size
// branches with a function pointer to the next branch in the chase.
int convert_pointers_to_branches(void *head, int chunk_size) {
  int remain = cycle_len(head);
  chunk_size = (remain < chunk_size)
                   ? remain
                   : remain / (1 << lround(log2(1.0 * remain / chunk_size)));
  int base_chunk_size = chunk_size;
  int chunks_remaining = remain / chunk_size;
  int chunk_count = 0;
  const int br_code_len = 12;  // len(mov_imm64) + max(len(jmp), len(ret))
  char *p = (char *)head;
  do {
    if (!chunk_count) chunk_count = remain / chunks_remaining;
    char *next = *((char **)p);
    for (int i = 8; i < br_code_len; i++) {
      if (p[i]) {
        fprintf(stderr, "not enough space to convert a pointer to branches\n");
        exit(1);
      }
    }
    p += x64_emit_mov_imm64_rax(p, (intptr_t)next);
    // At end of branch chunk, return with a pointer to the next chunk.
    --remain;
    if (--chunk_count == 0) {
      p += x64_emit_ret(p);
      --chunks_remaining;
    } else {
      p += x64_emit_jmp_to_rax(p);
    }
    p = next;
  } while (p != head);
  if (remain != 0 || chunks_remaining > 0) {
    fprintf(stderr, "internal error\n");
    exit(1);
  }
  return base_chunk_size;
}

#else
int convert_pointers_to_branches(void *head, int chunk_size) {
  fprintf(stderr, "Not implemented on this architecture.\n");
  exit(1);
}
#endif
