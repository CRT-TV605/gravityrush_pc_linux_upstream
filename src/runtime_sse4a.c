/* SSE4a (AMD only: EXTRQ, INSERTQ) on CPUs without it, such as every Intel CPU.
 *
 * The PS4's Jaguar CPU has SSE4a and the port runs the game's code natively: an EXTRQ/INSERTQ
 * stops an Intel CPU with an illegal instruction, and the fault handler emulates it here. */
#include "runtime.h"
#ifdef _WIN32
#include <windows.h>
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

#ifdef _WIN32
/* Emulates EXTRQ/INSERTQ at the faulting instruction (Windows CONTEXT XMM registers). */
int runtime_sse4a_emulate(void *context_record) {
    CONTEXT *c = context_record;
    const unsigned char *at = (const unsigned char *)(uintptr_t)c->Rip;
    Sse4a op;
    if (!decode(at, &op)) return 0;
    M128A *xmm = c->FltSave.XmmRegisters;
    uint64_t dst = xmm[op.dst].Low;
    int len = op.len, idx = op.idx;
    if (op.reg_form) {
        const uint64_t control = op.insert ? (uint64_t)xmm[op.src].High : xmm[op.src].Low;
        len = (int)(control & 63);
        idx = (int)((control >> 8) & 63);
    }
    const unsigned bits = len ? (unsigned)len : 64u;
    const uint64_t mask = bits == 64 ? ~0ull : ((1ull << bits) - 1);
    if (op.insert) {
        const uint64_t field = xmm[op.src].Low & mask;
        dst = (dst & ~(mask << idx)) | (field << idx);
    } else {
        dst = (dst >> idx) & mask;
    }
    xmm[op.dst].Low = dst;
    xmm[op.dst].High = 0; /* as AMD hardware leaves it (documented undefined) */
    c->Rip += (DWORD64)op.size;
    return 1;
}
#endif
