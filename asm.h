#ifndef ASM_H_INCLUDED
#define ASM_H_INCLUDED

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int convert_pointers_to_branches(void *head, int chunk_size);
int cycle_len(void *p);

#if defined(__aarch64__)
uint64_t rbits(uint64_t val, int rmask);
uint64_t rmask_lsh(uint64_t val, int rmask, int left_shift);
void arm_emit_adrp(char *p, int rd, int val21);
void arm_emit_adds_imm(char *p, int rd, int rn, int imm12);
void arm_emit_br(char *p, int rs);
void arm_emit_movk(char *p, int rd, int imm16, int shift16);
void arm_emit_movz(char *p, int rd, int imm16, int shift16);
void arm_emit_ret(char *p);

#elif defined(__x86_64__)
int x64_emit_mov_imm64_rax(char *p, uint64_t imm64);
int x64_emit_jmp_to_rax(char *p);
int x64_emit_ret(char *p);

#endif
#endif
