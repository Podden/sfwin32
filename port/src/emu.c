/* Vectored-exception emulation layer.
 *
 * Everything the game does that Win32 will not allow -- software interrupts,
 * port I/O, interrupt-flag changes, foreign segment selectors -- arrives here
 * as an exception. We decode the instruction at EIP, service it against the
 * CONTEXT registers, advance EIP, and resume.
 *
 * Measured on this host (see ../../vehtest):
 *   int nn, mov Sreg   -> 0xC0000005 STATUS_ACCESS_VIOLATION
 *   in/out/cli/sti     -> 0xC0000096 STATUS_PRIVILEGED_INSTRUCTION
 * so dispatch is on the opcode at EIP, never on the exception code.
 */

#include "port.h"

#include <mmsystem.h>     /* timeBeginPeriod: WIN32_LEAN_AND_MEAN hides it */
#include <stdio.h>
#include <string.h>

shadow_segs g_seg;
uint8_t *g_dosmem;

static int  g_exit_requested;
static int  g_exit_code;
static PVOID g_veh;

/* ------------------------------------------------------------ todo list */

#define TODO_MAX 256
static struct { const char *kind; uint32_t detail, eip, count; } g_todo[TODO_MAX];
static int g_todo_n;

void todo_hit(const char *kind, uint32_t detail, uint32_t eip)
{
    int i;
    for (i = 0; i < g_todo_n; i++)
        if (g_todo[i].kind == kind && g_todo[i].detail == detail) {
            g_todo[i].count++;
            return;
        }
    if (g_todo_n < TODO_MAX) {
        g_todo[g_todo_n].kind = kind;
        g_todo[g_todo_n].detail = detail;
        g_todo[g_todo_n].eip = eip;
        g_todo[g_todo_n].count = 1;
        g_todo_n++;
        LOGW("TODO %s 0x%X at eip=0x%08X (orig 0x%08X)",
             kind, detail, eip, eip - g_img.delta);
    }
}

void todo_dump(void)
{
    int i;
    if (!g_todo_n) { LOGI("no unimplemented sites were reached"); return; }
    LOGI("---- unimplemented sites reached (%d) ----", g_todo_n);
    for (i = 0; i < g_todo_n; i++)
        LOGI("  %-12s 0x%-6X  x%-6u first at orig 0x%08X",
             g_todo[i].kind, g_todo[i].detail, g_todo[i].count,
             g_todo[i].eip - g_img.delta);
}

/* -------------------------------------------------------- register access */

static DWORD *reg32(CONTEXT *c, int i)
{
    switch (i) {
    case 0: return &c->Eax; case 1: return &c->Ecx;
    case 2: return &c->Edx; case 3: return &c->Ebx;
    case 4: return &c->Esp; case 5: return &c->Ebp;
    case 6: return &c->Esi; default: return &c->Edi;
    }
}

/* 8-bit register: 0..3 = al,cl,dl,bl (low byte), 4..7 = ah,ch,dh,bh */
static void set_reg8(CONTEXT *c, int i, uint8_t v)
{
    DWORD *r = reg32(c, i & 3);
    if (i < 4) *r = (*r & 0xFFFFFF00u) | v;
    else       *r = (*r & 0xFFFF00FFu) | ((DWORD)v << 8);
}
static uint8_t get_reg8(CONTEXT *c, int i)
{
    DWORD r = *reg32(c, i & 3);
    return (uint8_t)(i < 4 ? r : (r >> 8));
}

/* ------------------------------------------------------- instruction form */

typedef struct {
    int      len;          /* total instruction length                    */
    int      op32;         /* operand size 32 (else 16)                   */
    int      rep;          /* 0xF3 present                                */
    int      seg;          /* segment override: -1 none, else 0..5        */
    uint8_t  op;           /* primary opcode                              */
    const uint8_t *p;      /* instruction bytes                           */
} insn;

enum { SEG_ES = 0, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS };

static int decode_prefixes(const uint8_t *p, insn *in)
{
    int n = 0;
    in->op32 = 1; in->rep = 0; in->seg = -1;
    for (;;) {
        uint8_t b = p[n];
        if      (b == 0x66) { in->op32 = 0; n++; }
        else if (b == 0x67) { n++; }
        else if (b == 0xF3) { in->rep = 1; n++; }
        else if (b == 0xF2) { in->rep = 2; n++; }
        else if (b == 0xF0) { n++; }
        else if (b == 0x26) { in->seg = SEG_ES; n++; }
        else if (b == 0x2E) { in->seg = SEG_CS; n++; }
        else if (b == 0x36) { in->seg = SEG_SS; n++; }
        else if (b == 0x3E) { in->seg = SEG_DS; n++; }
        else if (b == 0x64) { in->seg = SEG_FS; n++; }
        else if (b == 0x65) { in->seg = SEG_GS; n++; }
        else break;
    }
    in->p = p;
    in->op = p[n];
    return n;
}

/* Decode a modrm byte at p[0]; returns bytes consumed, fills ea/reg/mod. */
static int decode_modrm(CONTEXT *c, const uint8_t *p, uint32_t *ea,
                        int *regfield, int *mod, int *rm)
{
    uint8_t m = p[0];
    int n = 1;
    uint32_t addr = 0;

    *mod = m >> 6; *regfield = (m >> 3) & 7; *rm = m & 7;
    if (*mod == 3) return n;

    if (*rm == 4) {                                   /* SIB */
        uint8_t sib = p[n++];
        int scale = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        if (idx != 4) addr += *reg32(c, idx) << scale;
        if (base == 5 && *mod == 0) { addr += *(const uint32_t *)(p + n); n += 4; }
        else addr += *reg32(c, base);
    } else if (*rm == 5 && *mod == 0) {
        addr = *(const uint32_t *)(p + n); n += 4;
    } else {
        addr = *reg32(c, *rm);
    }

    if (*mod == 1)      { addr += (int32_t)(int8_t)p[n]; n += 1; }
    else if (*mod == 2) { addr += *(const int32_t *)(p + n); n += 4; }

    *ea = addr;
    return n;
}

/* The process's own selectors, captured at install time. Instructions that
 * merely *read* a segment register (`mov dx, ds`) never fault, so the game
 * sees Win32's real selector values and hands them straight back to us. Those
 * must be recognised as "the flat selector", not mistaken for a foreign one. */
static uint16_t g_real_sel[6];

static int is_flat_selector(uint16_t v)
{
    int i;
    if (v == SEL_FLAT) return 1;
    for (i = 0; i < 6; i++) if (v && v == g_real_sel[i]) return 1;
    return 0;
}

/* ---- LDT descriptors handed out through DPMI --------------------------
 * The game allocates selectors and sets their bases itself (int 31h AX=0007),
 * so we keep a table rather than hard-coding every selector value. A base is
 * a linear address in the game's world, which is resolved into one of ours. */

#define MAX_DESCR 64
static struct { uint16_t sel; uint32_t base_lin; int used, based; }
    g_descr[MAX_DESCR];
static uint16_t g_next_sel = 0x0040;

/* Demand-commit works for CPU accesses because they fault into the handler,
 * but a kernel call probes the buffer itself and fails with ERROR_NOACCESS
 * instead. So any buffer handed to the host has to be committed up front. */
void mem_ensure_committed(void *p, uint32_t n)
{
    uintptr_t a = (uintptr_t)p, lo = (uintptr_t)g_img.mem;
    uintptr_t start, end;

    if (!g_img.mem || a < lo || a >= lo + g_img.reserved) return;
    start = a & ~(uintptr_t)0xFFF;
    end   = (a + n + 0xFFF) & ~(uintptr_t)0xFFF;
    if (end > lo + g_img.reserved) end = lo + g_img.reserved;
    if (!VirtualAlloc((void *)start, end - start, MEM_COMMIT, PAGE_READWRITE))
        LOGW("commit %p..%p failed: %lu", (void *)start, (void *)end,
             GetLastError());
}

uint8_t *lin_to_ptr(uint32_t lin)
{
    if (lin < DOSMEM_SIZE) return g_dosmem + lin;
    if (lin >= g_img.obj1_base && lin < g_img.obj2_base + g_img.obj2_size)
        return (uint8_t *)gp(lin);
    return (uint8_t *)(uintptr_t)lin;          /* already one of our pointers */
}

uint16_t descr_alloc(int count)
{
    uint16_t first = 0;
    int i;
    while (count-- > 0) {
        for (i = 0; i < MAX_DESCR; i++) if (!g_descr[i].used) break;
        if (i == MAX_DESCR) { LOGW("out of LDT descriptors"); return first; }
        g_descr[i].used = 1;
        g_descr[i].sel  = g_next_sel;
        g_descr[i].base_lin = 0;
        g_descr[i].based = 0;
        if (!first) first = g_next_sel;
        g_next_sel += 8;
    }
    return first;
}

static int descr_find(uint16_t sel)
{
    int i;
    for (i = 0; i < MAX_DESCR; i++)
        if (g_descr[i].used && g_descr[i].sel == sel) return i;
    return -1;
}

void descr_set_base(uint16_t sel, uint32_t linear)
{
    int i = descr_find(sel);
    if (i < 0) {                                /* the game may set a base on
                                                 * a selector we handed out
                                                 * statically -- adopt it */
        for (i = 0; i < MAX_DESCR; i++) if (!g_descr[i].used) break;
        if (i == MAX_DESCR) return;
        g_descr[i].used = 1;
        g_descr[i].sel = sel;
    }
    /* Store the *linear* base, not a resolved pointer. A descriptor based at
     * linear 0 is the flat selector: its offsets are already linear addresses
     * in the game's world, so they have to be translated per access. Resolving
     * the base once and adding the offset sent large heap offsets far outside
     * the low-memory block and corrupted whatever they landed on. */
    g_descr[i].base_lin = linear;
    g_descr[i].based = 1;
    LOGI("descriptor 0x%04X base <- linear 0x%08X", sel, linear);
}

uint32_t descr_get_base(uint16_t sel)
{
    int i = descr_find(sel);
    return (i >= 0 && g_descr[i].based) ? g_descr[i].base_lin : 0;
}

static uint16_t get_seg(int which)
{
    switch (which) {
    case SEG_ES: return g_seg.es; case SEG_DS: return g_seg.ds;
    case SEG_FS: return g_seg.fs; case SEG_GS: return g_seg.gs;
    case SEG_SS: return g_seg.ss; default: return g_seg.cs;
    }
}

/* Map a segmented access onto something we can actually touch.
 *
 * DOS/4GW runs a flat model in which every selector the program sees is
 * zero-based: `sel:off` simply means linear address `off`, with the program's
 * own objects sitting at their link-time bases and the PSP / BIOS data down in
 * low memory. So an offset is an address in the *original* address space, and
 * translating it is just a question of which region it falls in.
 */
/* Is this address backed by committed memory? Used only on the fault path, so
 * the VirtualQuery cost does not matter. */
static int mem_present(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    return !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

/* Set while servicing a fault whose address fell below 1 MB. DOS/4GW's flat
 * selector is zero-based, so the game reads the BIOS data area and other low
 * memory through plain flat pointers -- addresses Win32 can never map. When
 * that happens the access is re-aimed at g_dosmem instead. */
static int g_map_low;

static void *xlat(int seg, uint32_t off, int *ok)
{
    uint16_t sel = get_seg(seg);
    *ok = 1;

    if (sel == SEL_FLAT) {                     /* untouched flat selector: the
                                                * offset is already one of our
                                                * real addresses */
        if (g_map_low && off < DOSMEM_SIZE) return g_dosmem + off;
        /* On the fault path the address may simply be wild. Emulating the
         * access would then fault *inside* the handler, which re-enters the
         * VEH and destroys its own frame -- so refuse it here and let the
         * fault be reported normally. */
        if (g_map_low && !mem_present((const void *)(uintptr_t)off)) {
            *ok = 0;
            return NULL;
        }
        return (void *)(uintptr_t)off;
    }

    if (sel == SEL_VIDEO)
        return g_window ? g_window + (off & 0xFFFF) : (*ok = 0, NULL);

    /* DOS/4GW hands the program fixed selectors for the PSP and for the
     * environment. The PSP one is zero-based (offsets are true linear
     * addresses); the environment one is based at the environment block. */
    if (sel == SEL_ENV)
        return (off < DOSMEM_ENV_SIZE) ? g_dosmem + DOSMEM_ENV + off
                                       : (*ok = 0, NULL);

    if (sel == SEL_DOSBUF)
        return (off < DOSMEM_XFER_SIZE) ? g_dosmem + DOSMEM_XFER + off
                                        : (*ok = 0, NULL);

    {   /* a descriptor the game allocated and based itself: base + offset is a
         * linear address in the game's world, translated like any other */
        int i = descr_find(sel);
        if (i >= 0 && g_descr[i].based)
            return lin_to_ptr(g_descr[i].base_lin + off);
    }

    /* inside the loaded image -> rebase it */
    if (off >= g_img.obj1_base && off < g_img.obj2_base + g_img.obj2_size)
        return gp(off);

    /* low memory: PSP, environment, BIOS data area */
    if (off < DOSMEM_SIZE)
        return g_dosmem + off;

    *ok = 0;
    return NULL;
}

/* ------------------------------------------------------------- port I/O */

static int do_in(CONTEXT *c, insn *in, uint16_t port, int width)
{
    uint32_t v = 0;
    ioport_in(port, width, &v);
    if (width == 1)      set_reg8(c, 0, (uint8_t)v);
    else if (width == 2) c->Eax = (c->Eax & 0xFFFF0000u) | (v & 0xFFFF);
    else                 c->Eax = v;
    return 1;
}

static int do_out(CONTEXT *c, insn *in, uint16_t port, int width)
{
    uint32_t v = (width == 1) ? (c->Eax & 0xFF)
               : (width == 2) ? (c->Eax & 0xFFFF) : c->Eax;
    ioport_out(port, width, v);
    return 1;
}

/* rep outs / rep ins -- used for VGA DAC palette uploads. */
static void do_string_port(CONTEXT *c, insn *in, int is_out, int width)
{
    uint32_t count = in->rep ? c->Ecx : 1;
    uint16_t port = (uint16_t)(c->Edx & 0xFFFF);
    int delta = (c->EFlags & 0x400) ? -width : width;   /* DF */
    uint32_t i;

    for (i = 0; i < count; i++) {
        int ok;
        if (is_out) {
            void *src = xlat(in->seg < 0 ? SEG_DS : in->seg, c->Esi, &ok);
            uint32_t v = 0;
            if (!ok) break;
            memcpy(&v, src, width);
            ioport_out(port, width, v);
            c->Esi += delta;
        } else {
            void *dst = xlat(SEG_ES, c->Edi, &ok);
            uint32_t v = 0;
            if (!ok) break;
            ioport_in(port, width, &v);
            memcpy(dst, &v, width);
            c->Edi += delta;
        }
    }
    if (in->rep) c->Ecx = 0;
}

/* --------------------------------------------- segmented memory emulation */

/* ---- flag helpers ---------------------------------------------------- */

#define F_CF 0x0001u
#define F_PF 0x0004u
#define F_AF 0x0010u
#define F_ZF 0x0040u
#define F_SF 0x0080u
#define F_OF 0x0800u

static uint32_t wmask(int w)
{
    return w == 1 ? 0xFFu : w == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}
static uint32_t wsign(int w)
{
    return w == 1 ? 0x80u : w == 2 ? 0x8000u : 0x80000000u;
}

static void flags_common(CONTEXT *c, uint32_t res, int w)
{
    uint32_t m = wmask(w), r = res & m;
    int i, par = 0;
    c->EFlags &= ~(F_ZF | F_SF | F_PF);
    if (!r) c->EFlags |= F_ZF;
    if (r & wsign(w)) c->EFlags |= F_SF;
    for (i = 0; i < 8; i++) if ((r >> i) & 1) par++;
    if (!(par & 1)) c->EFlags |= F_PF;
}
static void flags_logic(CONTEXT *c, uint32_t res, int w)
{
    c->EFlags &= ~(F_CF | F_OF | F_AF);
    flags_common(c, res, w);
}
static void flags_addsub(CONTEXT *c, uint32_t a, uint32_t b, uint32_t res,
                         int w, int sub)
{
    uint32_t m = wmask(w), s = wsign(w);
    c->EFlags &= ~(F_CF | F_OF | F_AF);
    if (sub) { if ((a & m) < (b & m)) c->EFlags |= F_CF; }
    else     { if ((res & m) < (a & m)) c->EFlags |= F_CF; }
    if (((a ^ b ^ res) & 0x10)) c->EFlags |= F_AF;
    if (sub) { if (((a ^ b) & (a ^ res) & s)) c->EFlags |= F_OF; }
    else     { if ((~(a ^ b) & (a ^ res) & s)) c->EFlags |= F_OF; }
    flags_common(c, res, w);
}

/* Apply one ALU operation, returning the result and setting flags. `op` is the
 * group-1 index: 0 add, 1 or, 2 adc, 3 sbb, 4 and, 5 sub, 6 xor, 7 cmp. */
static uint32_t alu(CONTEXT *c, int op, uint32_t a, uint32_t b, int w, int *store)
{
    uint32_t r;
    int cf = (c->EFlags & F_CF) ? 1 : 0;
    *store = 1;
    switch (op) {
    case 0: r = a + b;          flags_addsub(c, a, b, r, w, 0); break;
    case 1: r = a | b;          flags_logic(c, r, w);           break;
    case 2: r = a + b + cf;     flags_addsub(c, a, b, r, w, 0); break;
    case 3: r = a - b - cf;     flags_addsub(c, a, b, r, w, 1); break;
    case 4: r = a & b;          flags_logic(c, r, w);           break;
    case 5: r = a - b;          flags_addsub(c, a, b, r, w, 1); break;
    case 6: r = a ^ b;          flags_logic(c, r, w);           break;
    default: r = a - b;         flags_addsub(c, a, b, r, w, 1);
             *store = 0;        break;                          /* cmp */
    }
    return r;
}

static uint32_t rd_reg(CONTEXT *c, int i, int w)
{
    return (w == 1) ? get_reg8(c, i) : (*reg32(c, i) & wmask(w));
}
static void wr_reg(CONTEXT *c, int i, uint32_t v, int w)
{
    if (w == 1) set_reg8(c, i, (uint8_t)v);
    else if (w == 2) *reg32(c, i) = (*reg32(c, i) & 0xFFFF0000u) | (v & 0xFFFF);
    else *reg32(c, i) = v;
}

/* Emulate an instruction whose memory operand goes through a foreign selector.
 *
 * `mov` alone is not enough -- the startup walks the environment block with
 * `cmp byte [esi],0`, and any ALU form can appear. This covers the common
 * modrm-addressed opcodes; anything else is reported so it shows up in the
 * to-do list rather than crashing silently.
 *
 * Returns instruction length, or 0 if the encoding is not understood.
 */
static int emulate_seg_mov(CONTEXT *c, insn *in, int pfx)
{
    const uint8_t *p = in->p + pfx + 1;      /* past prefixes and opcode */
    int mod, rf, rm, n, ok, w, store;
    uint32_t ea, a, b, r;
    uint8_t op = in->op;
    void *ptr;

    /* two-byte opcodes: movzx / movsx, used to walk the VBE mode list */
    if (op == 0x0F) {
        uint8_t op2 = in->p[pfx + 1];
        int srcw;
        switch (op2) {
        case 0xB6: case 0xBE: srcw = 1; break;
        case 0xB7: case 0xBF: srcw = 2; break;
        default: return 0;
        }
        n = decode_modrm(c, in->p + pfx + 2, &ea, &rf, &mod, &rm);
        if (mod == 3) return 0;
        ptr = xlat(in->seg < 0 ? SEG_DS : in->seg, ea, &ok);
        if (!ok) return 0;
        a = 0;
        memcpy(&a, ptr, srcw);
        if      (op2 == 0xBE) a = (uint32_t)(int32_t)(int8_t)a;
        else if (op2 == 0xBF) a = (uint32_t)(int32_t)(int16_t)a;
        wr_reg(c, rf, a, in->op32 ? 4 : 2);
        return pfx + 2 + n;
    }

    /* segment-register store needs no ALU machinery */
    if (op == 0x8C) {
        uint16_t v;
        n = decode_modrm(c, p, &ea, &rf, &mod, &rm);
        if (mod == 3) return 0;
        ptr = xlat(in->seg < 0 ? SEG_DS : in->seg, ea, &ok);
        if (!ok) return 0;
        v = get_seg(rf);
        memcpy(ptr, &v, 2);
        return pfx + 1 + n;
    }

    /* ALU r/m,r and r,r/m : 00..3B with the low 3 bits selecting form */
    if (op < 0x40 && (op & 7) <= 3 && ((op >> 3) & 7) <= 7) {
        int g = (op >> 3) & 7, dir = (op >> 1) & 1;
        w = (op & 1) ? (in->op32 ? 4 : 2) : 1;
        n = decode_modrm(c, p, &ea, &rf, &mod, &rm);
        if (mod == 3) return 0;
        ptr = xlat(in->seg < 0 ? SEG_DS : in->seg, ea, &ok);
        if (!ok) return 0;
        a = 0; memcpy(&a, ptr, w);
        b = rd_reg(c, rf, w);
        if (dir) { uint32_t t = a; a = b; b = t; }     /* reg <- reg op mem */
        r = alu(c, g, a, b, w, &store);
        if (store) { if (dir) wr_reg(c, rf, r, w); else memcpy(ptr, &r, w); }
        return pfx + 1 + n;
    }

    switch (op) {
    case 0x80: case 0x81: case 0x83: {        /* group 1 with immediate */
        w = (op == 0x80) ? 1 : (in->op32 ? 4 : 2);
        n = decode_modrm(c, p, &ea, &rf, &mod, &rm);
        if (mod == 3) return 0;
        ptr = xlat(in->seg < 0 ? SEG_DS : in->seg, ea, &ok);
        if (!ok) return 0;
        if (op == 0x81) { b = *(const uint32_t *)(p + n); n += (w == 2) ? 2 : 4;
                          if (w == 2) b &= 0xFFFF; }
        else            { b = (uint32_t)(int32_t)(int8_t)p[n]; n += 1; }
        a = 0; memcpy(&a, ptr, w);
        r = alu(c, rf, a, b, w, &store);
        if (store) memcpy(ptr, &r, w);
        return pfx + 1 + n;
    }

    case 0x84: case 0x85: {                   /* test r/m, r */
        w = (op & 1) ? (in->op32 ? 4 : 2) : 1;
        n = decode_modrm(c, p, &ea, &rf, &mod, &rm);
        if (mod == 3) return 0;
        ptr = xlat(in->seg < 0 ? SEG_DS : in->seg, ea, &ok);
        if (!ok) return 0;
        a = 0; memcpy(&a, ptr, w);
        flags_logic(c, a & rd_reg(c, rf, w), w);
        return pfx + 1 + n;
    }

    case 0x88: case 0x89: case 0x8A: case 0x8B: {   /* mov */
        w = (op & 1) ? (in->op32 ? 4 : 2) : 1;
        n = decode_modrm(c, p, &ea, &rf, &mod, &rm);
        if (mod == 3) return 0;
        ptr = xlat(in->seg < 0 ? SEG_DS : in->seg, ea, &ok);
        if (!ok) return 0;
        if (op <= 0x89) { uint32_t v = rd_reg(c, rf, w); memcpy(ptr, &v, w); }
        else            { uint32_t v = 0; memcpy(&v, ptr, w);
                          wr_reg(c, rf, v, w); }
        return pfx + 1 + n;
    }

    case 0xC6: case 0xC7: {                   /* mov r/m, imm */
        w = (op & 1) ? (in->op32 ? 4 : 2) : 1;
        n = decode_modrm(c, p, &ea, &rf, &mod, &rm);
        if (mod == 3) return 0;
        ptr = xlat(in->seg < 0 ? SEG_DS : in->seg, ea, &ok);
        if (!ok) return 0;
        b = 0; memcpy(&b, p + n, w); n += w;
        memcpy(ptr, &b, w);
        return pfx + 1 + n;
    }

    case 0xF6: case 0xF7: {                   /* group 3 */
        w = (op & 1) ? (in->op32 ? 4 : 2) : 1;
        n = decode_modrm(c, p, &ea, &rf, &mod, &rm);
        if (mod == 3) return 0;
        ptr = xlat(in->seg < 0 ? SEG_DS : in->seg, ea, &ok);
        if (!ok) return 0;
        a = 0; memcpy(&a, ptr, w);
        if (rf == 0 || rf == 1) {             /* test imm */
            b = 0; memcpy(&b, p + n, w); n += w;
            flags_logic(c, a & b, w);
        } else if (rf == 2) { r = ~a; memcpy(ptr, &r, w); }
        else if (rf == 3)   { r = (uint32_t)(0 - a);
                              flags_addsub(c, 0, a, r, w, 1);
                              memcpy(ptr, &r, w); }
        else return 0;
        return pfx + 1 + n;
    }

    case 0xFE: case 0xFF: {                   /* inc / dec / push */
        w = (op & 1) ? (in->op32 ? 4 : 2) : 1;
        n = decode_modrm(c, p, &ea, &rf, &mod, &rm);
        if (mod == 3) return 0;
        ptr = xlat(in->seg < 0 ? SEG_DS : in->seg, ea, &ok);
        if (!ok) return 0;
        a = 0; memcpy(&a, ptr, w);
        if (rf == 0)      { uint32_t cf = c->EFlags & F_CF;
                            r = a + 1; flags_addsub(c, a, 1, r, w, 0);
                            c->EFlags = (c->EFlags & ~F_CF) | cf;
                            memcpy(ptr, &r, w); }
        else if (rf == 1) { uint32_t cf = c->EFlags & F_CF;
                            r = a - 1; flags_addsub(c, a, 1, r, w, 1);
                            c->EFlags = (c->EFlags & ~F_CF) | cf;
                            memcpy(ptr, &r, w); }
        else if (rf == 6) { c->Esp -= 4;
                            *(uint32_t *)(uintptr_t)c->Esp = a; }
        else return 0;
        return pfx + 1 + n;
    }

    default:
        return 0;
    }
}

/* String instructions take ES:EDI / DS:ESI implicitly, so they carry no
 * prefix to trap on -- they simply fault against a flat selector when the
 * game meant a segmented one (the PSP command tail, for instance). Emulate
 * the whole family, REP included. */
static int emulate_string(CONTEXT *c, insn *in, int pfx)
{
    int width, ok, delta;
    uint32_t count, mask;

    switch (in->op) {
    case 0xA4: case 0xA6: case 0xAA: case 0xAC: case 0xAE: width = 1; break;
    case 0xA5: case 0xA7: case 0xAB: case 0xAD: case 0xAF:
        width = in->op32 ? 4 : 2; break;
    default: return 0;
    }
    mask  = (width == 1) ? 0xFFu : (width == 2) ? 0xFFFFu : 0xFFFFFFFFu;
    delta = (c->EFlags & 0x400) ? -width : width;      /* DF */
    count = in->rep ? c->Ecx : 1;

    while (count) {
        void *s = NULL, *d = NULL;
        uint32_t a = 0, b = 0;

        switch (in->op) {                              /* source operand */
        case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAC: case 0xAD:
            s = xlat(in->seg < 0 ? SEG_DS : in->seg, c->Esi, &ok);
            if (!ok) return 0;
            break;
        default: break;
        }
        switch (in->op) {                              /* destination operand */
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:
        case 0xAA: case 0xAB: case 0xAE: case 0xAF:
            d = xlat(SEG_ES, c->Edi, &ok);
            if (!ok) return 0;
            break;
        default: break;
        }

        switch (in->op) {
        case 0xA4: case 0xA5:                          /* movs */
            memcpy(d, s, width); c->Esi += delta; c->Edi += delta; break;
        case 0xAA: case 0xAB:                          /* stos */
            memcpy(d, &c->Eax, width); c->Edi += delta; break;
        case 0xAC: case 0xAD:                          /* lods */
            memcpy(&b, s, width);
            c->Eax = (c->Eax & ~mask) | (b & mask);
            c->Esi += delta; break;
        case 0xA6: case 0xA7:                          /* cmps */
            memcpy(&a, s, width); memcpy(&b, d, width);
            c->Esi += delta; c->Edi += delta; break;
        case 0xAE: case 0xAF:                          /* scas */
            a = c->Eax & mask; memcpy(&b, d, width);
            c->Edi += delta; break;
        default: break;
        }

        count--;
        if (in->rep) c->Ecx = count;

        if (in->op == 0xA6 || in->op == 0xA7 ||
            in->op == 0xAE || in->op == 0xAF) {
            int zf = ((a & mask) == (b & mask));
            if (zf) c->EFlags |= 0x40u; else c->EFlags &= ~0x40u;
            if (a < b) c->EFlags |= 1u; else c->EFlags &= ~1u;
            if (in->rep == 1 && !zf) break;            /* repe  */
            if (in->rep == 2 &&  zf) break;            /* repne */
        }
    }
    return pfx + 1;
}

/* -------------------------------------------------------------- handler */

static int readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (SIZE_T)((const BYTE *)mbi.BaseAddress + mbi.RegionSize -
                    (const BYTE *)p) >= n;
}

/* Only *failing* segment loads reach us: `mov es, <valid flat selector>`
 * succeeds silently, so a shadow set earlier would go stale and we would keep
 * translating addresses that the game has already made flat again.
 *
 * While any shadow selector is foreign we therefore run with the trap flag
 * set, and on each single step we check whether the instruction that just
 * executed was a segment load -- if so the real register now holds the truth.
 * The window is short (a few instructions around PSP access), so this costs
 * nothing in the steady state. */
static uint32_t g_ss_prev_eip;

static uint16_t seg_from_context(CONTEXT *c, int which)
{
    switch (which) {
    case SEG_ES: return (uint16_t)c->SegEs; case SEG_CS: return (uint16_t)c->SegCs;
    case SEG_SS: return (uint16_t)c->SegSs; case SEG_DS: return (uint16_t)c->SegDs;
    case SEG_FS: return (uint16_t)c->SegFs; default: return (uint16_t)c->SegGs;
    }
}

static int shadow_dirty(void)
{
    return g_seg.es | g_seg.ds | g_seg.fs | g_seg.gs | g_seg.ss;
}

static uint32_t g_ss_count;
static uint32_t g_tf_dropped;    /* single steps caught inside our own code */

static void sync_tf(CONTEXT *c)
{
    if (shadow_dirty()) {
        c->EFlags |= 0x100u;
        g_ss_prev_eip = c->Eip;
        LOGT("TF on  (es=%04X ds=%04X) next=%08X", g_seg.es, g_seg.ds, c->Eip);
    } else {
        c->EFlags &= ~0x100u;
        LOGT("TF off at %08X", c->Eip);
    }
}

static void set_seg(int which, uint16_t val)
{
    if (is_flat_selector(val)) val = SEL_FLAT;
    switch (which) {
    case SEG_ES: g_seg.es = val; break;
    case SEG_DS: g_seg.ds = val; break;
    case SEG_FS: g_seg.fs = val; break;
    case SEG_GS: g_seg.gs = val; break;
    case SEG_SS: g_seg.ss = val; break;
    default: break;
    }
}

/* Breadcrumbs for the __except below. Reading `ep` from the except block has
 * proven unreliable once something has gone wrong, so record what we need in
 * globals as we go and report from those instead. */
static volatile uint32_t g_dbg_eip, g_dbg_code, g_dbg_op, g_dbg_ea;
static volatile int      g_dbg_stage;

static LONG emu_veh_inner(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;

    {   /* One-off: where does Windows put the exception frame relative to the
         * interrupted ESP? Injecting an interrupt frame at game_esp-12 is only
         * safe if the CONTEXT the dispatcher is about to consume lives below
         * that. */
        static int once;
        if (!once++) {
            int marker;
            LOGI("frame layout: CONTEXT=%p ERECORD=%p game_esp=0x%08lX "
                 "handler_esp~%p  (gap to context = %ld bytes)",
                 (void *)c, (void *)ep->ExceptionRecord, c->Esp,
                 (void *)&marker, (long)(c->Esp - (uintptr_t)c));
        }
    }

    g_dbg_stage = 1;
    g_dbg_eip = c->Eip;
    g_dbg_code = ep->ExceptionRecord->ExceptionCode;
    g_dbg_op = 0;
    g_dbg_ea = 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)c->Eip;
    insn in;
    int pfx, n, mod, rf, rm;
    uint32_t ea;

    /* A vectored handler is process-wide: it sees exceptions from the SDL
     * thread's renderer, the CRT, and anything else in the process. Only the
     * game's own code is ours to emulate -- everything else must be left to
     * whoever is expecting it, or we "service" someone else's handled
     * exception and corrupt them. Demand-committing our heap is the one thing
     * that is keyed on the faulting address rather than EIP, so it stays
     * outside this test. */
    {
        uintptr_t glo = (uintptr_t)g_img.mem;
        int in_game = g_img.mem && c->Eip >= glo &&
                      c->Eip < glo + g_img.obj1_size;
        /* One exception to "not ours, don't touch": a single step outside the
         * image is *always* ours, because we are the only thing in the process
         * that sets the trap flag.
         *
         * It happens because the game does not only fault into this handler --
         * it also jumps directly into code we substituted for its own. Each of
         * the 16 Miles hooks is a five-byte JMP out of game code into a C
         * function here. If TF happened to be armed for segment tracking at
         * that moment, every instruction of that C function traps, lands
         * outside the image, gets declined, and takes the process down with an
         * unhandled STATUS_SINGLE_STEP.
         *
         * DESKTOP.EXE hit this immediately where FIGHT.EXE never had: it calls
         * the sound API while a foreign selector is live. Drop TF for the
         * duration -- the tail of this handler re-arms it from shadow_dirty()
         * the next time the game itself faults, so nothing is lost but the few
         * steps spent inside our own code. */
        if (!in_game &&
            ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
            c->EFlags &= ~0x100u;
            if (++g_tf_dropped <= 12 || (g_tf_dropped % 5000) == 0)
                LOGT("TF dropped at %08X eflags=%08X tid=%lu (#%u)",
                     c->Eip, c->EFlags, GetCurrentThreadId(), g_tf_dropped);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (!in_game) {
            static uint32_t nother;
            if (++nother <= 12)
                LOGT("foreign exception code=%08X eip=%08X eflags=%08X tid=%lu",
                     ep->ExceptionRecord->ExceptionCode, c->Eip, c->EFlags,
                     GetCurrentThreadId());
        }

        if (!in_game &&
            ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
            return EXCEPTION_CONTINUE_SEARCH;
        if (!in_game) {
            /* Let the demand-commit path below have a look, then stop. */
            if (ep->ExceptionRecord->NumberParameters >= 2) {
                uintptr_t a = (uintptr_t)
                    ep->ExceptionRecord->ExceptionInformation[1];
                if (a >= glo && a < glo + g_img.reserved) {
                    void *page = (void *)(a & ~(uintptr_t)0xFFF);
                    if (VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE))
                        return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    /* ---- single step: did the last instruction reload a segment? ---- */
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        const uint8_t *q = (const uint8_t *)(uintptr_t)g_ss_prev_eip;
        if (g_ss_prev_eip && readable(q, 8)) {
            insn pi;
            int ppfx = decode_prefixes(q, &pi), which = -1;
            if (pi.op == 0x8E) {
                int m, r, rm2; uint32_t e2;
                decode_modrm(c, q + ppfx + 1, &e2, &r, &m, &rm2);
                which = r;
            }
            else if (pi.op == 0x07) which = SEG_ES;
            else if (pi.op == 0x17) which = SEG_SS;
            else if (pi.op == 0x1F) which = SEG_DS;
            if (which >= 0) set_seg(which, seg_from_context(c, which));

            /* `mov r16, Sreg` does not fault either, so the game just read
             * Win32's real selector where it expected the foreign one it
             * loaded earlier. Put the shadow value back. */
            else if (pi.op == 0x8C) {
                int m, r, rm2; uint32_t e2;
                decode_modrm(c, q + ppfx + 1, &e2, &r, &m, &rm2);
                if (m == 3) {
                    uint16_t sv = get_seg(r);
                    if (sv != SEL_FLAT) {
                        DWORD *d = reg32(c, rm2);
                        *d = (*d & 0xFFFF0000u) | sv;
                    }
                }
            }
            /* likewise `push Sreg`: rewrite the value it just pushed */
            else if (pi.op == 0x06 || pi.op == 0x0E ||
                     pi.op == 0x16 || pi.op == 0x1E) {
                int wh = (pi.op == 0x06) ? SEG_ES : (pi.op == 0x0E) ? SEG_CS
                       : (pi.op == 0x16) ? SEG_SS : SEG_DS;
                uint16_t sv = get_seg(wh);
                if (sv != SEL_FLAT)
                    *(uint16_t *)(uintptr_t)c->Esp = sv;
            }
        }
        if (g_ss_count++ < 60)
            LOGT("SS #%u prev=%08X (orig %08X) now=%08X es=%04X ds=%04X",
                 g_ss_count, g_ss_prev_eip, g_ss_prev_eip - g_img.delta,
                 c->Eip, g_seg.es, g_seg.ds);
        g_ss_prev_eip = c->Eip;
        if (!shadow_dirty()) c->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* ---- demand-commit the heap ----
     * The runtime grows its break past the top we advertised, so rather than
     * guess how much it will want, the region is reserved generously and
     * pages are committed the first time the game touches them. Resume without
     * advancing EIP so the faulting instruction simply runs again. */
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t lo = (uintptr_t)g_img.mem;
        if (a >= lo && a < lo + g_img.reserved) {
            void *page = (void *)(a & ~(uintptr_t)0xFFF);
            if (VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE)) {
                LOGT("committed heap page at %p (orig 0x%08X)",
                     page, (uint32_t)(uintptr_t)page - g_img.delta);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            LOGE("commit at %p failed: %lu", page, GetLastError());
        }
    }

    if (!readable(p, 16)) {
        LOGE("fault at unreadable EIP: code=0x%08lX eip=0x%08lX (orig 0x%08X) "
             "addr=0x%08lX",
             ep->ExceptionRecord->ExceptionCode, c->Eip,
             c->Eip - g_img.delta,
             (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    pfx = decode_prefixes(p, &in);

    switch (in.op) {

    /* ---- software interrupts ---- */
    case 0xCD: {
        uint8_t vec = p[pfx + 1];
        int handled = 0;
        c->Eip += pfx + 2;                    /* advance first: handlers may
                                                 redirect control flow      */
        switch (vec) {
        case 0x21: handled = dos_int21(c);  break;
        case 0x31: handled = dpmi_int31(c); break;
        case 0x2F: handled = dpmi_int2f(c); break;
        case 0x10: handled = vbe_dispatch(c); break;
        case 0x33: handled = mouse_int33(c); break;
        default: break;
        }
        if (!handled) todo_hit("int", vec, c->Eip - (pfx + 2));
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* ---- port I/O ---- */
    case 0xE4: do_in (c, &in, p[pfx + 1], 1); c->Eip += pfx + 2; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xE5: do_in (c, &in, p[pfx + 1], in.op32 ? 4 : 2); c->Eip += pfx + 2; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xE6: do_out(c, &in, p[pfx + 1], 1); c->Eip += pfx + 2; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xE7: do_out(c, &in, p[pfx + 1], in.op32 ? 4 : 2); c->Eip += pfx + 2; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xEC: do_in (c, &in, (uint16_t)c->Edx, 1); c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xED: do_in (c, &in, (uint16_t)c->Edx, in.op32 ? 4 : 2); c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xEE: do_out(c, &in, (uint16_t)c->Edx, 1); c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0xEF: do_out(c, &in, (uint16_t)c->Edx, in.op32 ? 4 : 2); c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;

    case 0x6C: do_string_port(c, &in, 0, 1); c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0x6D: do_string_port(c, &in, 0, in.op32 ? 4 : 2); c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0x6E: do_string_port(c, &in, 1, 1); c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0x6F: do_string_port(c, &in, 1, in.op32 ? 4 : 2); c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;

    /* ---- interrupt flag: meaningless here ---- */
    case 0xFA: case 0xFB:
        c->Eip += pfx + 1;
        return EXCEPTION_CONTINUE_EXECUTION;

    /* ---- mov Sreg, r/m16 ---- */
    case 0x8E: {
        uint16_t val;
        n = decode_modrm(c, p + pfx + 1, &ea, &rf, &mod, &rm);
        if (mod == 3) val = (uint16_t)*reg32(c, rm);
        else {
            int ok;
            void *ptr = xlat(in.seg < 0 ? SEG_DS : in.seg, ea, &ok);
            if (!ok || !readable(ptr, 2)) { todo_hit("movseg_src", ea, c->Eip);
                                            return EXCEPTION_CONTINUE_SEARCH; }
            val = *(uint16_t *)ptr;
        }
        set_seg(rf, val);
        LOGT("mov seg%d, 0x%04X at orig 0x%08X", rf, val, c->Eip - g_img.delta);
        c->Eip += pfx + 1 + n;
        sync_tf(c);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    /* ---- pop Sreg ---- */
    case 0x07: set_seg(SEG_ES, *(uint16_t *)(uintptr_t)c->Esp); c->Esp += in.op32 ? 4 : 2; c->Eip += pfx + 1; sync_tf(c); return EXCEPTION_CONTINUE_EXECUTION;
    case 0x17: set_seg(SEG_SS, *(uint16_t *)(uintptr_t)c->Esp); c->Esp += in.op32 ? 4 : 2; c->Eip += pfx + 1; sync_tf(c); return EXCEPTION_CONTINUE_EXECUTION;
    case 0x1F: set_seg(SEG_DS, *(uint16_t *)(uintptr_t)c->Esp); c->Esp += in.op32 ? 4 : 2; c->Eip += pfx + 1; sync_tf(c); return EXCEPTION_CONTINUE_EXECUTION;

    /* ---- push Sreg (pushes our shadow value) ---- */
    case 0x06: c->Esp -= in.op32 ? 4 : 2; *(uint32_t *)(uintptr_t)c->Esp = g_seg.es; c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0x0E: c->Esp -= in.op32 ? 4 : 2; *(uint32_t *)(uintptr_t)c->Esp = g_seg.cs; c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0x16: c->Esp -= in.op32 ? 4 : 2; *(uint32_t *)(uintptr_t)c->Esp = g_seg.ss; c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;
    case 0x1E: c->Esp -= in.op32 ? 4 : 2; *(uint32_t *)(uintptr_t)c->Esp = g_seg.ds; c->Eip += pfx + 1; return EXCEPTION_CONTINUE_EXECUTION;

    default: break;
    }

    /* ---- memory the host cannot address directly ----
     * Either a foreign selector is live, or the access landed in the low
     * megabyte that DOS/4GW exposed through its zero-based flat selector. */
    {
        uintptr_t fa = (ep->ExceptionRecord->NumberParameters >= 2)
                     ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1]
                     : ~(uintptr_t)0;
        int seg_live = (in.seg >= 0 || g_seg.ds != SEL_FLAT ||
                        g_seg.es != SEL_FLAT);
        /* Windows does not always hand back a usable faulting address -- some
         * faults report 0xFFFFFFFF -- so do not gate the low-memory redirect
         * on it. Enable it for any access violation and let xlat decide from
         * the effective address the decoder computes; it only redirects
         * offsets below 1 MB and passes everything else through untouched.
         *
         * This matters because the game genuinely reads low addresses on
         * purpose: e.g. at 0x69BC8 an index of -1 sets the pointer to 0 and
         * the code still dereferences it, discarding the value. Under
         * DOS/4GW's flat selector that read the interrupt vector table and
         * was harmless. */
        int low = (ep->ExceptionRecord->ExceptionCode ==
                   EXCEPTION_ACCESS_VIOLATION);
        (void)fa;

        if (seg_live || low) {
            g_map_low = low;
            g_dbg_op = in.op;
            g_dbg_ea = (uint32_t)fa;
            if ((in.op >= 0xA4 && in.op <= 0xA7) ||
                (in.op >= 0xAA && in.op <= 0xAF)) {
                g_dbg_stage = 7;
                n = emulate_string(c, &in, pfx);
            } else {
                g_dbg_stage = 8;
                n = emulate_seg_mov(c, &in, pfx);
            }
            g_dbg_stage = 9;
            g_map_low = 0;
            if (n) { c->Eip += n; return EXCEPTION_CONTINUE_EXECUTION; }
            todo_hit(low ? "lowmem" : "segmem", in.op, c->Eip);
        }
    }

    /* A stale exception from the external-injection race.
     *
     * If the kernel already had a fault pending for the game thread when the
     * timer thread redirected EIP, it delivers that fault afterwards --
     * describing the original access, but pointing at the interrupt handler we
     * just entered. The give-away is an exception reported exactly at an
     * installed ISR entry, which ordinary execution never reaches (the game
     * gets there only by our injection, and by then the frame is already
     * pushed).
     *
     * Discarding it is correct rather than merely convenient: the interrupted
     * instruction's address is on the ISR's own stack frame, so it runs again
     * when the handler IRETDs -- and faults again, at a point where it can be
     * serviced properly. */
    if (c->Eip == dos_get_ivec(8) || c->Eip == dos_get_ivec(9)) {
        static uint32_t stale;
        if (++stale <= 4 || (stale & 0xFFF) == 0)
            LOGW("discarded stale exception at ISR entry 0x%08X (orig 0x%08X), "
                 "count %u", c->Eip, c->Eip - g_img.delta, stale);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    LOGE("unhandled fault: code=0x%08lX eip=0x%08lX (orig 0x%08X) op=0x%02X",
         ep->ExceptionRecord->ExceptionCode, c->Eip, c->Eip - g_img.delta, in.op);
    todo_dump();
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Windows hands the single-step exception back with TF already cleared, and
 * an ordinary fault clears it too. So the trap flag has to be re-armed on
 * every resume for as long as a foreign selector is live -- doing it in one
 * place here is the only way to keep it consistent across the handler's many
 * exit paths. */
static void irq_deliver_pending(CONTEXT *c);   /* defined with the timer code */

/* Last time the game took an exception. Drives the choice between in-handler
 * and external interrupt delivery. */
static volatile LONG g_fault_tick;

static uint32_t g_last_ok_eip, g_handled;

/* Debug aid: poll a global (given as its original link-time address) on every
 * serviced fault and report when it changes. Cheap, and it answers the only
 * question that matters when an init flag never gets set -- whether anything
 * writes it at all, and roughly where. */
int g_dump_keys;                 /* --dumpkeys: log what the game thinks is held */
int g_extern_irq = 1;            /* --no-extern-irq disables the fallback path  */

uint32_t g_watch_addr;
int      g_watch_dwords = 1;     /* --watch ADDR:N covers a whole struct */
#define WATCH_MAX 64
static uint32_t g_watch_last[WATCH_MAX];
static int      g_watch_primed;

/* Watching a single dword answers "did this change"; watching a struct answers
 * the more useful question "was this object touched at all", which is what
 * separates "never initialised" from "initialised but one field left zero". */
static void watch_poll(CONTEXT *c)
{
    int i, n = g_watch_dwords;
    if (!g_watch_addr) return;
    if (n < 1) n = 1;
    if (n > WATCH_MAX) n = WATCH_MAX;

    if (!g_watch_primed) {
        for (i = 0; i < n; i++)
            g_watch_last[i] = ((volatile uint32_t *)gp(g_watch_addr))[i];
        g_watch_primed = 1;
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t v = ((volatile uint32_t *)gp(g_watch_addr))[i];
        if (v != g_watch_last[i]) {
            LOGI("WATCH 0x%08X+%02X: 0x%08X -> 0x%08X  "
                 "(near orig 0x%08X, fault #%u)",
                 g_watch_addr, i * 4, g_watch_last[i], v,
                 c->Eip - g_img.delta, g_handled);
            g_watch_last[i] = v;
        }
    }
}

/* A vectored handler is re-entered for exceptions raised *inside* it. Without
 * a guard, one bad access in the emulator recurses until the stack -- and this
 * function's own frame -- is destroyed, and the eventual report is garbage
 * (the symptom was `ep->ContextRecord` reading back as 0x23). Per-thread
 * because the SDL and timer threads run through here too. */
static __declspec(thread) int g_veh_depth;

static LONG CALLBACK emu_veh(EXCEPTION_POINTERS *ep)
{
    LONG r;

    /* Clear the trap flag for *this handler's own* execution.
     *
     * Windows does not clear TF when it dispatches an exception to user mode:
     * the handler inherits the interrupted context's flags. Since the emulator
     * deliberately runs the game with TF set whenever a shadow selector is
     * foreign, any fault taken during that window drops us in here still in
     * single-step mode -- and then every instruction of this handler raises
     * STATUS_SINGLE_STEP. The nested ones arrive with g_veh_depth already
     * non-zero, get declined below, and take the process down.
     *
     * FIGHT.EXE survived on luck: its faults during a TF window are rare.
     * DESKTOP.EXE scans its environment block with ES foreign and faults on
     * every byte, so it died within a second of starting, every time. */
    __asm {
        pushfd
        and  dword ptr [esp], 0xFFFFFEFF
        popfd
    }

    if (g_veh_depth) {
        static uint32_t nnest;
        if (++nnest <= 12)
            LOGW("NESTED exception declined: code=%08X eip=%08X eflags=%08X "
                 "tid=%lu (#%u)",
                 ep->ExceptionRecord->ExceptionCode,
                 ep->ContextRecord->Eip, ep->ContextRecord->EFlags,
                 GetCurrentThreadId(), nnest);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_veh_depth++;

    /* If the handler itself faults, Windows tears the process down without
     * running any filter, and the log ends mid-sentence with no explanation.
     * Catch that here so the failure is at least reported. */
    __try {
        r = emu_veh_inner(ep);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LOGE("=== the emulator's own handler faulted (stage %d) ===",
             g_dbg_stage);
        LOGE("  was servicing eip=0x%08X (orig 0x%08X) code=0x%08X op=0x%02X "
             "faultaddr=0x%08X",
             g_dbg_eip, g_dbg_eip - g_img.delta, g_dbg_code, g_dbg_op,
             g_dbg_ea);
        LOGE("  shadow es=%04X ds=%04X | handled %u, last ok orig 0x%08X",
             g_seg.es, g_seg.ds, g_handled, g_last_ok_eip - g_img.delta);
        todo_dump();
        g_map_low = 0;
        g_veh_depth--;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_veh_depth--;

    if (r == EXCEPTION_CONTINUE_EXECUTION) {
        CONTEXT *c = ep->ContextRecord;
        uintptr_t glo = (uintptr_t)g_img.mem;
        int resuming_game = g_img.mem && c->Eip >= glo &&
                            c->Eip < glo + g_img.obj1_size;

        /* Only single-step the *game*. Re-arming TF for a resume into our own
         * code is what made DESKTOP.EXE unrunnable: an exception taken while
         * TF was armed leaves the handler itself in single-step mode, and this
         * line put the flag straight back after the handler had just dropped
         * it -- so execution crawled one instruction per exception through our
         * own code until something declined a nested trap and the process
         * died. The shadow is unaffected; the next fault in game code re-arms
         * it from shadow_dirty() as before. */
        if (shadow_dirty() && resuming_game) {
            c->EFlags |= 0x100u; g_ss_prev_eip = c->Eip;
        } else {
            c->EFlags &= ~0x100u;
        }
        /* Hardware interrupts are delivered here, on the context we are about
         * to hand back, rather than by rewriting a running thread from
         * outside. */
        irq_deliver_pending(c);
        g_fault_tick = (LONG)GetTickCount();   /* the game is still faulting */
        g_last_ok_eip = c->Eip;
        watch_poll(c);
        /* Once the game is painting straight into the linear framebuffer it
         * may make no further VBE calls, so drive presentation and SDL event
         * handling from here rather than relying on the game to ask. */
        if (g_vid_active && (g_handled & 0x3FF) == 0) video_pump();
        if (++g_handled % 20000 == 0)
            LOGI("progress: %u faults serviced, now at orig 0x%08X",
                 g_handled, c->Eip - g_img.delta);
    }
    return r;
}

/* Last line of defence: whatever the VEH declined ends up here, so the log
 * always says where the game actually died. */
static LONG WINAPI emu_crash_filter(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    LOGE("=== CRASH ===");
    LOGE("code=0x%08lX eip=0x%08lX (orig 0x%08X)",
         ep->ExceptionRecord->ExceptionCode, c->Eip, c->Eip - g_img.delta);
    if (ep->ExceptionRecord->NumberParameters >= 2)
        LOGE("access %s address 0x%08lX",
             ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
             (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
    LOGE("eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX",
         c->Eax, c->Ebx, c->Ecx, c->Edx);
    LOGE("esi=%08lX edi=%08lX ebp=%08lX esp=%08lX",
         c->Esi, c->Edi, c->Ebp, c->Esp);
    LOGE("shadow: es=%04X ds=%04X fs=%04X gs=%04X",
         g_seg.es, g_seg.ds, g_seg.fs, g_seg.gs);
    LOGE("handled %u faults; last resumed at 0x%08X (orig 0x%08X)",
         g_handled, g_last_ok_eip, g_last_ok_eip - g_img.delta);

    {   /* Dump the objects the faulting instruction was walking. Register
         * values alone rarely say which field was null. */
        static const struct { const char *name; DWORD val; } r[] = {
            { "eax", 0 }, { "ebx", 0 }, { "esi", 0 }, { "edi", 0 }
        };
        DWORD vals[4]; const char *names[4] = { "eax", "ebx", "esi", "edi" };
        int k, j;
        (void)r;
        vals[0] = c->Eax; vals[1] = c->Ebx; vals[2] = c->Esi; vals[3] = c->Edi;
        for (k = 0; k < 4; k++) {
            uintptr_t p = (uintptr_t)vals[k];
            if (!readable((const void *)p, 0x90)) continue;
            for (j = 0; j < 0x90; j += 0x20) {
                const uint32_t *w = (const uint32_t *)(p + j);
                LOGE("  %s+%02X: %08X %08X %08X %08X %08X %08X %08X %08X",
                     names[k], j, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
    }
    todo_dump();
    log_shutdown();
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ------------------------------------------------------- timer interrupt */

/* The game installs a handler on vector 8 and expects IRQ0 to fire at the rate
 * it programs into the PIT. Nothing in the emulator drives that: once the game
 * is painting into the linear framebuffer it stops faulting entirely, so there
 * is no moment where we naturally regain control.
 *
 * So the tick is delivered the way the hardware would have: a separate thread
 * suspends the game thread, pushes a real interrupt frame onto its stack,
 * points EIP at the handler and resumes it. The handler ends in IRETD, which
 * pops the frame and returns to whatever was interrupted -- no different from
 * a genuine interrupt. IRETD returning to the same privilege level is legal in
 * user mode, so nothing needs emulating on the way back.
 */

/* Byte spans of the two handlers we drive, used to avoid re-entering either:
 * vector 8 covers 0xA8ED2..0xA9075, vector 9 covers 0x2BC22..0x2BD8A. */
#define ISR8_SPAN 0x1B0u
#define ISR9_SPAN 0x170u

static HANDLE g_game_thread;
static HANDLE g_timer_thread;
static volatile LONG g_ticks_delivered, g_ticks_skipped, g_irq1_delivered;

/* Writing below the game thread's ESP has to be done carefully: we are not
 * that thread, so touching its guard page would fault here instead of growing
 * its stack. */
static int stack_writable(uintptr_t addr, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)addr, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                         PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) return 0;
    return (SIZE_T)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - addr) >= n;
}

/* Is EIP inside either handler we drive? Real hardware masks an IRQ until its
 * handler acknowledges it, and both handlers have a window around their
 * `mov ss` / `mov esp` pair where the stack pointer is inconsistent. */
static int eip_in_isr(uint32_t eip)
{
    uint32_t h8 = dos_get_ivec(8), h9 = dos_get_ivec(9);
    if (h8 && eip >= h8 && eip < h8 + ISR8_SPAN) return 1;
    /* The int 9 handler runs on the interrupted stack rather than its own, so
     * re-entering it would also corrupt its saved registers. */
    if (h9 && eip >= h9 && eip < h9 + ISR9_SPAN) return 1;
    return 0;
}

/* Pending hardware interrupts. The timer thread only raises these flags; the
 * delivery happens inside the exception handler.
 *
 * Rewriting a *running* thread's context from outside cannot work here. The
 * game faults thousands of times a second, so there is almost always an
 * exception already pending in the kernel when the timer thread calls
 * SuspendThread/SetThreadContext -- and when the kernel then delivers it, the
 * exception record describes the original fault while the context carries our
 * redirected EIP. The observed symptom was an access violation "reading
 * address 0" reported at the first instruction of the keyboard ISR.
 *
 * Delivering from inside the handler avoids the race entirely: the CONTEXT is
 * ours until we return, so the frame we push and the EIP we set are applied
 * atomically by the same NtContinue that resumes the game. And because the
 * game faults constantly there is no shortage of delivery points. */
static volatile LONG g_pend_irq0, g_pend_irq1;
static volatile LONG g_skip_eip, g_skip_isr, g_skip_shadow,
                     g_skip_ctx, g_skip_stack;

static void irq_deliver_pending(CONTEXT *c)
{
    uintptr_t lo = (uintptr_t)g_img.mem, hi = lo + g_img.obj1_size;
    uint32_t handler, *sp;
    int vec;

    if (c->Eip < lo || c->Eip >= hi) { g_skip_eip++; return; }
    if (eip_in_isr(c->Eip))          { g_skip_isr++; return; }
    /* Never interrupt a single-step window: the trap flag is tracking a
     * foreign segment selector, and an interrupt taken here would leave the
     * shadow stale and the next translated access pointing at wrong memory. */
    if (shadow_dirty())              { g_skip_shadow++; return; }

    if      (g_pend_irq1 > 0 && kbd_pending()) vec = 9;
    else if (g_pend_irq0 > 0)                  vec = 8;
    else return;

    handler = dos_get_ivec(vec);
    if (handler < lo || handler >= hi) {       /* not installed yet */
        InterlockedExchange(vec == 9 ? &g_pend_irq1 : &g_pend_irq0, 0);
        return;
    }

    /* The frame goes just below the interrupted ESP. That is far above the
     * exception frame the dispatcher is about to consume (measured ~2.7 KB of
     * clearance), but check rather than assume. */
    if ((uintptr_t)c->Esp < (uintptr_t)c + sizeof(CONTEXT) + 64) {
        g_skip_ctx++; return;
    }
    if (!stack_writable((uintptr_t)c->Esp - 16, 16)) { g_skip_stack++; return; }

    c->Esp -= 12;                              /* EIP, CS, EFLAGS */
    sp = (uint32_t *)(uintptr_t)c->Esp;
    sp[0] = c->Eip;
    sp[1] = c->SegCs;
    sp[2] = c->EFlags;           /* what IRETD restores: keep it exactly as it
                                  * was, TF included -- clearing it here would
                                  * silently end a single-step window */
    c->EFlags &= ~0x100u;        /* but the handler itself must not step */
    c->Eip = handler;

    if (vec == 9) {
        InterlockedDecrement(&g_pend_irq1);
        InterlockedIncrement(&g_irq1_delivered);
    } else {
        InterlockedDecrement(&g_pend_irq0);
        InterlockedIncrement(&g_ticks_delivered);
    }
}

/* Fallback for when the game stops faulting.
 *
 * In-handler delivery cannot reach a loop that takes no exceptions -- and the
 * game has such loops, waiting on a timer tick that then never arrives, which
 * deadlocks it. Rewriting the context from outside can reach it, at the cost
 * of the race described above: if the kernel already has an exception pending
 * for the thread, it will later deliver that exception with our redirected
 * EIP.
 *
 * The two failure modes are complementary. This path is used *only* when the
 * thread has not faulted for several milliseconds, which is precisely the
 * state in which no exception can be pending -- so the race it is prone to
 * cannot occur, and the case in-handler delivery cannot serve is covered. */
/* Note: arming the trap flag from outside instead of redirecting EIP was tried
 * here, on the theory that only code owning the context should touch EIP/ESP.
 * It is prettier and it measured worse -- delivery fell to ~75 Hz because the
 * in-handler path is blocked too often by the segment-shadow guard, and it
 * crashed inside half a minute. The redirect below survived two independent
 * eight-minute soaks at a steady 200 Hz, so that is what ships. The race it is
 * exposed to is neutralised where the stale exception surfaces, in the handler.
 */
static void irq_inject_external(void)
{
    CONTEXT c;
    uintptr_t lo = (uintptr_t)g_img.mem, hi = lo + g_img.obj1_size;
    uint32_t handler, *sp;
    int vec;

    if (!g_game_thread) return;
    if (shadow_dirty()) return;

    if      (g_pend_irq1 > 0 && kbd_pending()) vec = 9;
    else if (g_pend_irq0 > 0)                  vec = 8;
    else return;

    handler = dos_get_ivec(vec);
    if (handler < lo || handler >= hi) return;

    if (SuspendThread(g_game_thread) == (DWORD)-1) return;

    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_SEGMENTS;
    if (!GetThreadContext(g_game_thread, &c) ||
        c.Eip < lo || c.Eip >= hi || eip_in_isr(c.Eip) ||
        !stack_writable((uintptr_t)c.Esp - 16, 16)) {
        ResumeThread(g_game_thread);
        return;
    }

    c.Esp -= 12;
    sp = (uint32_t *)(uintptr_t)c.Esp;
    sp[0] = c.Eip;
    sp[1] = c.SegCs;
    sp[2] = c.EFlags;
    c.EFlags &= ~0x100u;
    c.Eip = handler;
    c.ContextFlags = CONTEXT_CONTROL;

    if (SetThreadContext(g_game_thread, &c)) {
        if (vec == 9) { InterlockedDecrement(&g_pend_irq1);
                        InterlockedIncrement(&g_irq1_delivered); }
        else          { InterlockedDecrement(&g_pend_irq0);
                        InterlockedIncrement(&g_ticks_delivered); }
    }
    ResumeThread(g_game_thread);
}

static DWORD WINAPI timer_thread_main(LPVOID arg)
{
    LARGE_INTEGER freq, next, now;
    uint32_t last_div = 0;

    (void)arg;
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);

    for (;;) {
        uint32_t div;
        double hz;
        LONGLONG period;

        Sleep(1);

        /* Keyboard is raised on every pass, not just on a timer deadline, so
         * input latency is the ~1 ms sleep rather than one PIT period. Both
         * counters are capped: a stall must not turn into a burst of
         * back-to-back interrupts once delivery resumes. */
        if (kbd_pending() && g_pend_irq1 < 8) InterlockedIncrement(&g_pend_irq1);

        /* If the game has not faulted recently it is inside a loop the
         * in-handler path cannot reach, so go in from outside. */
        if (g_extern_irq && (g_pend_irq0 || g_pend_irq1) &&
            (LONG)(GetTickCount() - (DWORD)g_fault_tick) > 3)
            irq_inject_external();

        QueryPerformanceCounter(&now);
        if (now.QuadPart < next.QuadPart) continue;

        /* Honour whatever rate the game programmed into the PIT. */
        div = *(volatile uint32_t *)gp(GAME_PIT_DIVISOR_ADDR) & 0xFFFF;
        if (div == 0) div = 0x10000;
        if (div != last_div) {
            LOGI("timer rate: divisor %u -> %.2f Hz", div, 1193182.0 / div);
            last_div = div;
        }
        hz = 1193182.0 / (double)div;
        period = (LONGLONG)((double)freq.QuadPart / hz);
        if (period < 1) period = 1;

        /* Advance the deadline by exactly one period so the *average* rate
         * matches what the game asked for -- resetting it to "now + period"
         * would fold each wake-up's scheduling latency into the interval and
         * run the game's clock permanently slow. Resynchronise only if we
         * have fallen far enough behind that catching up would burst. */
        next.QuadPart += period;
        if (next.QuadPart < now.QuadPart - period * 4)
            next.QuadPart = now.QuadPart + period;

        if (g_pend_irq0 < 4) InterlockedIncrement(&g_pend_irq0);
        else                 InterlockedIncrement(&g_ticks_skipped);

        {   /* Debug aid: what does the game currently believe is held down?
             * Its int 9 ISR keeps a key-state array at 0x15DFCF indexed by
             * scancode (extended keys at 0x57 + table index), bit 0 = down. */
            static LARGE_INTEGER t_keys;
            if (g_dump_keys && g_img.mem) {
                if (!t_keys.QuadPart) t_keys = now;
                if (now.QuadPart - t_keys.QuadPart > freq.QuadPart * 2) {
                    const uint8_t *ks = (const uint8_t *)gp(0x0015DFCFu);
                    /* Consumers do not use that address directly: they load a
                     * pointer from 0x15DFCB and index *that* by scancode. If
                     * the pointer is stale the game reads someone else's
                     * memory as key state. */
                    uint32_t ptr = *(uint32_t *)gp(0x0015DFCBu);
                    const uint8_t *via = lin_to_ptr(ptr);
                    char buf[256], buf2[256];
                    int k, o = 0, o2 = 0;
                    buf[0] = buf2[0] = '\0';
                    for (k = 0; k < 0x80 && o < 200; k++)
                        if (ks[k] & 1) o += sprintf(buf + o, "%02X ", k);
                    for (k = 0; k < 0x80 && o2 < 200; k++)
                        if (via[k] & 1) o2 += sprintf(buf2 + o2, "%02X ", k);
                    LOGI("keystate: direct=[%s] ptr[0x15DFCB]=0x%08X "
                         "(expect 0x%08X) via-ptr=[%s] joyreads=%u",
                         buf, ptr, (uint32_t)(uintptr_t)gp(0x0015DFCFu), buf2,
                         g_joy_reads);
                    t_keys = now;
                }
            }
        }

        {   /* Report the achieved rate so a mismatch with the programmed one
             * is visible rather than silently degrading the game's timing. */
            static LARGE_INTEGER t_report;
            if (!t_report.QuadPart) t_report = now;
            if (now.QuadPart - t_report.QuadPart > freq.QuadPart * 5) {
                static LONG last_d, last_s;
                LONG d = g_ticks_delivered, s = g_ticks_skipped;
                double secs = (double)(now.QuadPart - t_report.QuadPart)
                            / (double)freq.QuadPart;
                LOGI("timer: %.1f Hz delivered, %.1f Hz skipped "
                     "(want %.1f Hz; totals %ld/%ld); IRQ1 total %ld",
                     (d - last_d) / secs, (s - last_s) / secs, hz, d, s,
                     g_irq1_delivered);
                /* Why in-handler delivery declined. Not a problem in itself --
                 * the external path covers those cases -- but the balance is
                 * worth being able to see. */
                LOGT("  deliver skips: eip=%ld isr=%ld shadow=%ld ctx=%ld "
                     "stack=%ld", g_skip_eip, g_skip_isr, g_skip_shadow,
                     g_skip_ctx, g_skip_stack);
                last_d = d; last_s = s; t_report = now;
            }
        }
    }
}

void timer_start(void)
{
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &g_game_thread,
                         0, FALSE, DUPLICATE_SAME_ACCESS)) {
        LOGE("cannot duplicate game thread handle: %lu", GetLastError());
        return;
    }
    g_timer_thread = CreateThread(NULL, 0, timer_thread_main, NULL, 0, NULL);
    if (!g_timer_thread) { LOGE("timer thread failed: %lu", GetLastError());
                           return; }
    LOGI("timer thread started (idles until vector 8 is installed)");
}

void timer_stats(uint32_t *delivered, uint32_t *skipped)
{
    if (delivered) *delivered = (uint32_t)g_ticks_delivered;
    if (skipped)   *skipped   = (uint32_t)g_ticks_skipped;
}

/* ------------------------------------------------------------------ entry */

void emu_request_exit(int code)
{
    g_exit_requested = 1;
    g_exit_code = code;
    LOGI("game requested exit with code %d", code);
    todo_dump();
    log_shutdown();
    video_shutdown();
    ExitProcess((UINT)code);
}

int emu_install(void)
{
    g_dosmem = (uint8_t *)VirtualAlloc(NULL, DOSMEM_SIZE,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_dosmem) { LOGE("dosmem alloc failed"); return 0; }
    memset(g_dosmem, 0, DOSMEM_SIZE);

    /* A minimal PSP at linear 0, which is where a zero-based selector points.
     * The startup reads the environment segment at +0x2C and parses the
     * command tail at +0x80/+0x81. */
    *(uint16_t *)(g_dosmem + 0x2C) = 0x0800;       /* env "segment" -> 0x8000 */

    /* The startup reads the top of available memory from +0x5C through the
     * zero-based selector and makes it the C runtime's break. Left at zero it
     * concludes there is no memory at all and dies with
     * "Not enough memory to allocate file structures". */
    *(uint32_t *)(g_dosmem + 0x5C) = g_img.heap_top;
    LOGI("reported memory top = 0x%08X", g_img.heap_top);

    /* BIOS data area at 0040:0000. The game reads it through flat pointers,
     * e.g. the keyboard LED byte at 0x497 during shutdown. */
    *(uint16_t *)(g_dosmem + 0x410) = 0x0021;      /* equipment flags        */
    g_dosmem[0x417] = 0;                           /* keyboard shift flags   */
    g_dosmem[0x418] = 0;
    g_dosmem[0x449] = 0x03;                        /* current video mode     */
    *(uint16_t *)(g_dosmem + 0x44A) = 80;          /* text columns           */
    *(uint32_t *)(g_dosmem + 0x46C) = 0;           /* timer tick count       */
    g_dosmem[0x497] = 0;                           /* keyboard LED state     */

    {   /* DOS command tail: length byte, text, CR. The leading space is what
         * the startup's argv splitter expects. FIGHT.EXE refuses to run
         * unless argv[1] is "solo" -- that is how SF.EXE launches it. */
        char tail[128];
        size_t n;
        _snprintf(tail, sizeof tail, " %s", g_cmdline ? g_cmdline : "");
        tail[sizeof tail - 1] = '\0';
        n = strlen(tail);
        if (n > 126) n = 126;
        g_dosmem[0x80] = (uint8_t)n;
        memcpy(g_dosmem + 0x81, tail, n);
        g_dosmem[0x81 + n] = 0x0D;
        LOGI("command tail: '%s'", tail);
    }
    memcpy(g_dosmem + 0x8000, "PATH=C:\\\0\0\1\0" "C:\\FIGHT.EXE\0", 25);

    {   /* record Win32's own selectors so we never mistake one for foreign */
        uint16_t es_, cs_, ss_, ds_, fs_, gs_;
        __asm {
            mov ax, es
            mov es_, ax
            mov ax, cs
            mov cs_, ax
            mov ax, ss
            mov ss_, ax
            mov ax, ds
            mov ds_, ax
            mov ax, fs
            mov fs_, ax
            mov ax, gs
            mov gs_, ax
        }
        g_real_sel[SEG_ES] = es_; g_real_sel[SEG_CS] = cs_;
        g_real_sel[SEG_SS] = ss_; g_real_sel[SEG_DS] = ds_;
        g_real_sel[SEG_FS] = fs_; g_real_sel[SEG_GS] = gs_;
        LOGI("host selectors: cs=%04X ds=%04X es=%04X ss=%04X fs=%04X gs=%04X",
             cs_, ds_, es_, ss_, fs_, gs_);
    }

    g_seg.es = g_seg.ds = g_seg.ss = SEL_FLAT;
    g_seg.fs = g_seg.gs = SEL_FLAT;
    g_seg.cs = SEL_FLAT;

    g_veh = AddVectoredExceptionHandler(1, emu_veh);
    if (!g_veh) { LOGE("AddVectoredExceptionHandler failed"); return 0; }
    SetUnhandledExceptionFilter(emu_crash_filter);
    return 1;
}

/* Transfer control to the game. DOS/4GW hands over with a flat address space
 * and a stack it chose; we keep the Win32 thread stack instead (linked large)
 * so that host calls made from the handler never run on a stack the OS does
 * not own. The startup code re-aligns ESP itself. */
#pragma warning(push)
#pragma warning(disable: 4731)   /* EBP modified by inline asm: intended --
                                  * this function jumps away and never returns */
void emu_enter_game(void)
{
    uint32_t entry = g_img.entry;
    LOGI("entering game at 0x%08X", entry);
    __asm {
        mov  eax, entry
        xor  ebx, ebx
        xor  ecx, ecx
        xor  edx, edx
        xor  esi, esi
        xor  edi, edi
        xor  ebp, ebp
        jmp  eax
    }
}
#pragma warning(pop)
