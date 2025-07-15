#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "br_asm.h"

static int cycle_len(void *p) {
  int count = 0;
  char *next = (char *)p;
  do {
    ++count;
    next = *(char **)next;
  } while (next != p);
  return count;
}

#if defined(__aarch64__)

static uint64_t rbits(uint64_t val, int rmask) {
  return val & ((1ULL << rmask) - 1);
}

static uint64_t rmask_lsh(uint64_t val, int rmask, int left_shift) {
  return (val & ((1ULL << rmask) - 1)) << left_shift;
}

static void arm_emit_br(char *p, int rs) {
  *(int *)p = 0b11010110000111110000000000000000 | rmask_lsh(rs, 5, 5);
}

static void arm_emit_movk(char *p, int rd, int imm16, int shift16) {
  *(int *)p = (0b111100101 << 23) | rmask_lsh(shift16, 16, 21) |
              rmask_lsh(imm16, 16, 5) | rmask_lsh(rd, 5, 0);
}

static void arm_emit_movz(char *p, int rd, int imm16, int shift16) {
  *(int *)p = (0b110100101 << 23) | rmask_lsh(shift16, 16, 21) |
              rmask_lsh(imm16, 16, 5) | rmask_lsh(rd, 5, 0);
}

static void arm_emit_ret(char *p) { *(int *)p = 0xd65f03c0; }

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
  return base_chunk_size;
}

#elif defined(__riscv) && __riscv_xlen == 64
static char *riscv64_emit_lui_a0_imm64(char *p, uint64_t imm64) {
  *p++ = 0x37; // opcode for LUI (Load Upper Immediate)
  *p++ = 0x05; // rd = a0
  *p++ = (imm64 >> 32) & 0xff;
  *p++ = (imm64 >> 40) & 0xff;
  *p++ = (imm64 >> 48) & 0xff;
  *p++ = (imm64 >> 56) & 0xff;
  return p;
}

static char *riscv64_emit_jalr_a0(char *p) {
  *p++ = 0x67; // opcode for JALR (Jump and Link Register)
  *p++ = 0x80; // rd = x1 (return address), rs1 = a0
  *p++ = 0x00;
  *p++ = 0x00;
  return p;
}

static char *riscv64_emit_ret(char *p) {
  *p++ = 0x80; // opcode for RETL (Return)
  *p++ = 0x02;
  *p++ = 0x10;
  *p++ = 0x00;
  return p;
}

int convert_pointers_to_branches(void *head, int chunk_size) {
  int remain = cycle_len(head);
  chunk_size = (remain < chunk_size)
                   ? remain
                   : remain / (1 << lround(log2(1.0 * remain / chunk_size)));
  int base_chunk_size = chunk_size;
  int chunks_remaining = remain / chunk_size;
  int chunk_count = 0;
  const int br_code_len = 20; // len(lui) + len(jalr)
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
    p = riscv64_emit_lui_a0_imm64(p, (intptr_t)next);
    --remain;
    if (--chunk_count == 0) {
      p = riscv64_emit_ret(p);
      --chunks_remaining;
    } else {
      p = riscv64_emit_jalr_a0(p);
    }
    p = next;
  } while (p != head);
  return base_chunk_size;
}

#elif defined(__x86_64__)

static char *x64_emit_mov_imm64_rax(char *p, uint64_t imm64) {
  *p++ = 0x48;  // movabs
  *p++ = 0xb8;
  for (int i = 0; i < 8; i++)
    *p++ = imm64 >> (8 * i);  // immediate value (8 bytes)
  return p;
}

static char *x64_emit_jmp_to_rax(char *p) {
  *p++ = 0xff;  // jmp *rax
  *p++ = 0xe0;
  return p;
}

static char *x64_emit_ret(char *p) {
  *p++ = 0xc3;
  return p;
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
    p = x64_emit_mov_imm64_rax(p, (intptr_t)next);
    // At end of branch chunk, return with a pointer to the next chunk.
    --remain;
    if (--chunk_count == 0) {
      p = x64_emit_ret(p);
      --chunks_remaining;
    } else {
      p = x64_emit_jmp_to_rax(p);
    }
    p = next;
  } while (p != head);
  return base_chunk_size;
}

#else
int convert_pointers_to_branches(void *head, int chunk_size) {
  fprintf(stderr, "Not implemented on this architecture.\n");
  exit(1);
}
#endif
