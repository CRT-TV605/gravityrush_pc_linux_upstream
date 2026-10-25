/* SSE4a (AMD only: EXTRQ, INSERTQ) on CPUs without it, such as every Intel CPU.
 *
 * The PS4's Jaguar CPU has SSE4a and the port runs the game's code natively: an EXTRQ/INSERTQ
 * stops an Intel CPU with an illegal instruction, and the fault handler emulates it here. */
#ifndef _WIN32
#define _GNU_SOURCE /* REG_RIP */
#endif
#include "runtime.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#endif

/* Decoded INSERTQ/EXTRQ at `at`: 0 when not one. */
typedef struct { int insert, reg_form, dst, src, len, idx, size; } Sse4a;
static int decode(const unsigned char *at, Sse4a *out) {
    int p = 0, rex = 0;
    if (at[p] != 0x66 && at[p] != 0xf2) return 0;
    out->insert = at[p++] == 0xf2;
    if ((at[p] & 0xf0) == 0x40) rex = at[p++];
    if (at[p++] != 0x0f) return 0;
    const unsigned char op = at[p++];
    if (op != 0x78 && op != 0x79) return 0;
    const unsigned char modrm = at[p++];
    if ((modrm >> 6) != 3) return 0;
    const int reg = ((modrm >> 3) & 7) | ((rex & 4) << 1), rm = (modrm & 7) | ((rex & 1) << 3);
    out->reg_form = op == 0x79;
    if (out->insert) { out->dst = reg; out->src = rm; }
    else {
        if (!out->reg_form && reg != 0) return 0; /* EXTRQ xmm, imm, imm is /0 */
        out->dst = out->reg_form ? reg : rm; out->src = rm;
    }
    out->len = out->idx = 0;
    if (!out->reg_form) { out->len = at[p] & 63; out->idx = at[p + 1] & 63; p += 2; }
    out->size = p;
    return 1;
}

/* Emulates the instruction at *rip on the 16 saved XMM registers (16 bytes each, low half
 * first: Windows' M128A and Linux's _libc_xmmreg alike) and steps over it. */
static int emulate(uint64_t *rip, unsigned char *xmm_registers) {
    const unsigned char *at = (const unsigned char *)(uintptr_t)*rip;
    Sse4a op;
    if (!decode(at, &op)) return 0;
    uint64_t xmm_dst[2], xmm_src[2];
    memcpy(xmm_dst, xmm_registers + 16 * op.dst, 16);
    memcpy(xmm_src, xmm_registers + 16 * op.src, 16);
    uint64_t dst = xmm_dst[0];
    int len = op.len, idx = op.idx;
    if (op.reg_form) {
        const uint64_t control = op.insert ? xmm_src[1] : xmm_src[0];
        len = (int)(control & 63);
        idx = (int)((control >> 8) & 63);
    }
    const unsigned bits = len ? (unsigned)len : 64u;
    const uint64_t mask = bits == 64 ? ~0ull : ((1ull << bits) - 1);
    if (op.insert) {
        const uint64_t field = xmm_src[0] & mask;
        dst = (dst & ~(mask << idx)) | (field << idx);
    } else {
        dst = (dst >> idx) & mask;
    }
    xmm_dst[0] = dst;
    xmm_dst[1] = 0; /* as AMD hardware leaves it (documented undefined) */
    memcpy(xmm_registers + 16 * op.dst, xmm_dst, 16);
    *rip += (uint64_t)op.size;
    return 1;
}

#ifdef _WIN32
/* Emulates EXTRQ/INSERTQ at the faulting instruction (Windows CONTEXT XMM registers). */
int runtime_sse4a_emulate(void *context_record) {
    CONTEXT *c = context_record;
    return emulate((uint64_t *)&c->Rip, (unsigned char *)c->FltSave.XmmRegisters);
}
#else
/* The same from the SIGILL handler (probe.c), on the signal's saved context. */
int runtime_sse4a_emulate(void *context) {
    ucontext_t *uc = context;
    if (!uc->uc_mcontext.fpregs) return 0;
    return emulate((uint64_t *)&uc->uc_mcontext.gregs[REG_RIP], (unsigned char *)uc->uc_mcontext.fpregs->_xmm);
}
#endif
