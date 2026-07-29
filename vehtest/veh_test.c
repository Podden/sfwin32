/* Feasibility test for the FIGHT.EXE static port.
 *
 * Everything in PLAN.md rests on five assumptions. This checks each one on the
 * actual host OS instead of trusting folklore:
 *
 *   1. VirtualAlloc can reserve the LE object bases 0x10000 and 0x140000.
 *   2. `int 0x21` executed in a 32-bit process raises a catchable exception.
 *   3. `in` / `out` raise a catchable exception.
 *   4. `cli` / `sti` raise a catchable exception.
 *   5. A vectored handler can service all of the above by editing CONTEXT and
 *      resuming -- including writing results back into the faulting registers.
 *
 * Plus one question that came out of the object layout: obj1 covers 0xA0000,
 * so the game cannot reach video memory through a flat pointer and must use a
 * DPMI selector. Test 6 asks what happens to `mov es, <selector>` here.
 *
 * The payload runs from the real object-1 base, not from .text, so this
 * exercises the actual configuration.
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define OBJ1_BASE 0x00010000u
#define OBJ1_SIZE 0x0012B822u
#define OBJ2_BASE 0x00140000u
#define OBJ2_SIZE 0x00036500u

/* ------------------------------------------------------------------ state */

static int g_int_hits, g_in_hits, g_out_hits, g_flag_hits, g_seg_hits;
static int g_unhandled;
static DWORD g_last_exception_code;
static WORD g_out_port;
static DWORD g_out_value;

#define LOGN 32
static struct { DWORD code, eip; BYTE op; const char *what; } g_log[LOGN];
static int g_logn;

static void logev(DWORD code, DWORD eip, BYTE op, const char *what)
{
    if (g_logn < LOGN) {
        g_log[g_logn].code = code;
        g_log[g_logn].eip = eip;
        g_log[g_logn].op = op;
        g_log[g_logn].what = what;
        g_logn++;
    }
}

/* Reading [Eip] inside the handler must not fault itself. */
static int readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (SIZE_T)((const BYTE *)mbi.BaseAddress + mbi.RegionSize -
                    (const BYTE *)p) >= n;
}

/* ---------------------------------------------------------------- handler */

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    const BYTE *p = (const BYTE *)c->Eip;
    int len = 0, op32 = 1;
    BYTE op;

    g_last_exception_code = ep->ExceptionRecord->ExceptionCode;

    if (!readable(p, 8)) return EXCEPTION_CONTINUE_SEARCH;

    for (;;) {                                   /* skip prefixes */
        BYTE b = p[len];
        if (b == 0x66) { op32 = 0; len++; continue; }
        if (b == 0xF3 || b == 0xF2 || b == 0x2E || b == 0x3E ||
            b == 0x26 || b == 0x36 || b == 0x64 || b == 0x65) { len++; continue; }
        break;
    }
    op = p[len];

    switch (op) {
    case 0xCD:                                   /* int imm8 */
        g_int_hits++;
        logev(g_last_exception_code, c->Eip, op, "int imm8");
        if (p[len + 1] == 0x21)                  /* stand-in for a DOS call */
            c->Eax = 0x11220000;
        c->Eip += len + 2;
        return EXCEPTION_CONTINUE_EXECUTION;

    case 0xE4:                                   /* in al, imm8 */
        g_in_hits++;
        logev(g_last_exception_code, c->Eip, op, "in al, imm8");
        c->Eax = (c->Eax & 0xFFFFFF00u) | 0xA5;
        c->Eip += len + 2;
        return EXCEPTION_CONTINUE_EXECUTION;

    case 0xE6:                                   /* out imm8, al */
        g_out_hits++;
        logev(g_last_exception_code, c->Eip, op, "out imm8, al");
        g_out_port = p[len + 1];
        g_out_value = c->Eax & 0xFF;
        c->Eip += len + 2;
        return EXCEPTION_CONTINUE_EXECUTION;

    case 0xEC:                                   /* in al, dx */
        g_in_hits++;
        logev(g_last_exception_code, c->Eip, op, "in al, dx");
        c->Eax = (c->Eax & 0xFFFFFF00u) | 0x09;
        c->Eip += len + 1;
        return EXCEPTION_CONTINUE_EXECUTION;

    case 0xED:                                   /* in eax/ax, dx */
        g_in_hits++;
        logev(g_last_exception_code, c->Eip, op, "in eax, dx");
        if (op32) c->Eax = 0xDEADBEEF;
        else      c->Eax = (c->Eax & 0xFFFF0000u) | 0xBEEF;
        c->Eip += len + 1;
        return EXCEPTION_CONTINUE_EXECUTION;

    case 0xEE:                                   /* out dx, al */
        g_out_hits++;
        logev(g_last_exception_code, c->Eip, op, "out dx, al");
        g_out_port = (WORD)(c->Edx & 0xFFFF);
        g_out_value = c->Eax & 0xFF;
        c->Eip += len + 1;
        return EXCEPTION_CONTINUE_EXECUTION;

    case 0xEF:                                   /* out dx, eax/ax */
        g_out_hits++;
        logev(g_last_exception_code, c->Eip, op, "out dx, eax");
        g_out_port = (WORD)(c->Edx & 0xFFFF);
        g_out_value = op32 ? c->Eax : (c->Eax & 0xFFFF);
        c->Eip += len + 1;
        return EXCEPTION_CONTINUE_EXECUTION;

    case 0xFA:                                   /* cli */
    case 0xFB:                                   /* sti */
        g_flag_hits++;
        logev(g_last_exception_code, c->Eip, op, op == 0xFA ? "cli" : "sti");
        c->Eip += len + 1;
        return EXCEPTION_CONTINUE_EXECUTION;

    case 0x8E:                                   /* mov Sreg, r/m16 */
        g_seg_hits++;
        logev(g_last_exception_code, c->Eip, op, "mov Sreg, r/m");
        /* Cannot honour a foreign selector under Win32 -- skip it and see
         * whether the process survives. modrm is mod=11 in our payload. */
        c->Eip += len + 2;
        return EXCEPTION_CONTINUE_EXECUTION;

    default:
        g_unhandled++;
        logev(g_last_exception_code, c->Eip, op, "UNHANDLED");
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

/* ---------------------------------------------------------------- payload */

/* Executed at OBJ1_BASE. Mirrors the shapes actually present in FIGHT.EXE:
 * a DOS call whose result lands in EAX, a VGA DAC write, a status-register
 * read, and an interrupt-flag pair.
 *
 *   mov eax, 0x3000        ; AH=30h, "get DOS version"
 *   int 0x21               ; -> handler sets EAX = 0x11220000
 *   mov edx, 0x3C8         ; VGA DAC write index
 *   mov al,  0x2A
 *   out dx,  al            ; -> handler captures port 0x3C8, value 0x2A
 *   mov edx, 0x3DA         ; VGA input status 1
 *   in  al,  dx            ; -> handler sets AL = 0x09
 *   cli
 *   sti
 *   ret                    ; EAX = 0x11220009 if every step worked
 */
static const BYTE payload[] = {
    0xB8, 0x00, 0x30, 0x00, 0x00,
    0xCD, 0x21,
    0xBA, 0xC8, 0x03, 0x00, 0x00,
    0xB0, 0x2A,
    0xEE,
    0xBA, 0xDA, 0x03, 0x00, 0x00,
    0xEC,
    0xFA,
    0xFB,
    0xC3
};

/*   mov ax, 0x00A7   ; a selector we were never given
 *   mov es, ax
 *   mov eax, 0x600D
 *   ret
 */
static const BYTE seg_payload[] = {
    0x66, 0xB8, 0xA7, 0x00,
    0x8E, 0xC0,
    0xB8, 0x0D, 0x60, 0x00, 0x00,
    0xC3
};

typedef DWORD (*payload_fn)(void);

/* ------------------------------------------------------------------- main */

static int fails;

static void check(const char *what, int ok, const char *detail)
{
    printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
    if (!ok) fails++;
}

int main(void)
{
    char buf[128];
    void *o1, *o2;
    DWORD old, r;
    payload_fn fn;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);   /* survive a hard fault mid-test */
    printf("VEH feasibility test -- %u-bit process\n\n",
           (unsigned)(sizeof(void *) * 8));
    if (sizeof(void *) != 4) {
        printf("must be built for Win32 (x86). Configure with -A Win32.\n");
        return 2;
    }

    {   /* What does the low address space actually look like here? */
        SYSTEM_INFO si;
        MEMORY_BASIC_INFORMATION mbi;
        BYTE *a = 0;
        GetSystemInfo(&si);
        printf("[0] address space\n");
        printf("    min app addr = %p   max app addr = %p   gran = 0x%lX\n",
               si.lpMinimumApplicationAddress, si.lpMaximumApplicationAddress,
               si.dwAllocationGranularity);
        printf("    map below 0x00400000:\n");
        while ((DWORD_PTR)a < 0x00400000u &&
               VirtualQuery(a, &mbi, sizeof mbi) == sizeof mbi) {
            const char *st = mbi.State == MEM_FREE    ? "FREE"
                           : mbi.State == MEM_RESERVE ? "RESERVE"
                                                      : "COMMIT";
            const char *ty = mbi.Type == MEM_IMAGE   ? " IMAGE"
                           : mbi.Type == MEM_MAPPED  ? " MAPPED"
                           : mbi.Type == MEM_PRIVATE ? " PRIVATE" : "";
            printf("      %p .. %p  %-7s%-8s prot=0x%04lX\n",
                   mbi.BaseAddress,
                   (BYTE *)mbi.BaseAddress + mbi.RegionSize - 1,
                   st, ty, mbi.Protect);
            a = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
            if (mbi.RegionSize == 0) break;
        }
        printf("\n");
    }

    printf("[1] can we honour the original LE bases?\n");
    o1 = VirtualAlloc((LPVOID)OBJ1_BASE, OBJ1_SIZE, MEM_RESERVE | MEM_COMMIT,
                      PAGE_READWRITE);
    sprintf(buf, "err=%lu (487 = ERROR_INVALID_ADDRESS)",
            o1 ? 0 : GetLastError());
    check("original base 0x10000 is NOT available (expected)",
          o1 == NULL, buf);
    if (o1) VirtualFree(o1, 0, MEM_RELEASE);

    o2 = VirtualAlloc((LPVOID)OBJ2_BASE, OBJ2_SIZE, MEM_RESERVE | MEM_COMMIT,
                      PAGE_READWRITE);
    sprintf(buf, "err=%lu", o2 ? 0 : GetLastError());
    check("original base 0x140000 is NOT available (expected)",
          o2 == NULL, buf);
    if (o2) VirtualFree(o2, 0, MEM_RELEASE);

    sprintf(buf, "obj1 would span 0x%08X-0x%08X", OBJ1_BASE,
            OBJ1_BASE + OBJ1_SIZE);
    check("...and 0xA0000 falls inside obj1 anyway",
          0xA0000u >= OBJ1_BASE && 0xA0000u < OBJ1_BASE + OBJ1_SIZE, buf);

    printf("\n[1b] rebasing instead: let the OS choose\n");
    o1 = VirtualAlloc(NULL, OBJ1_SIZE, MEM_RESERVE | MEM_COMMIT,
                      PAGE_READWRITE);
    sprintf(buf, "obj1 at %p (delta 0x%08lX)", o1,
            (DWORD)((DWORD_PTR)o1 - OBJ1_BASE));
    check("obj1 mapped at an OS-chosen address", o1 != NULL, buf);

    o2 = VirtualAlloc(NULL, OBJ2_SIZE, MEM_RESERVE | MEM_COMMIT,
                      PAGE_READWRITE);
    sprintf(buf, "obj2 at %p (delta 0x%08lX)", o2,
            (DWORD)((DWORD_PTR)o2 - OBJ2_BASE));
    check("obj2 mapped at an OS-chosen address", o2 != NULL, buf);

    if (!o1) { printf("\ncannot continue without obj1\n"); return 1; }

    if (!AddVectoredExceptionHandler(1, veh)) {
        printf("AddVectoredExceptionHandler failed: %lu\n", GetLastError());
        return 1;
    }

    printf("\n[2] executing the payload from the mapped object-1 region\n");
    memcpy(o1, payload, sizeof payload);
    memcpy((BYTE *)o1 + 0x100, seg_payload, sizeof seg_payload);
    if (!VirtualProtect(o1, 0x1000, PAGE_EXECUTE_READ, &old)) {
        printf("VirtualProtect failed: %lu\n", GetLastError());
        return 1;
    }
    FlushInstructionCache(GetCurrentProcess(), o1, sizeof payload);

    fn = (payload_fn)o1;
    r = fn();

    sprintf(buf, "%d site(s), code=0x%08lX", g_int_hits, g_last_exception_code);
    check("int 0x21 faulted and was resumed", g_int_hits == 1, buf);
    sprintf(buf, "port=0x%03X value=0x%02X (want 0x3C8 / 0x2A)",
            g_out_port, g_out_value);
    check("out dx, al captured port and value",
          g_out_hits == 1 && g_out_port == 0x3C8 && g_out_value == 0x2A, buf);
    sprintf(buf, "%d site(s)", g_in_hits);
    check("in al, dx faulted and was resumed", g_in_hits == 1, buf);
    sprintf(buf, "%d site(s)", g_flag_hits);
    check("cli / sti faulted and were resumed", g_flag_hits == 2, buf);
    sprintf(buf, "EAX = 0x%08lX (want 0x11220009)", r);
    check("register write-back survived resumption", r == 0x11220009u, buf);

    printf("\n[3] foreign segment selector\n");
    g_seg_hits = 0;
    fn = (payload_fn)((BYTE *)o1 + 0x100);
    r = fn();
    sprintf(buf, "%s, returned 0x%08lX",
            g_seg_hits ? "FAULTED (must be emulated)"
                       : "did not fault (loaded silently)", r);
    check("process survived a foreign selector load", r == 0x600Du, buf);

    printf("\nexception log:\n");
    for (i = 0; i < g_logn; i++)
        printf("    code=0x%08lX  eip=0x%08lX  op=0x%02X  %s\n",
               g_log[i].code, g_log[i].eip, g_log[i].op, g_log[i].what);
    if (g_unhandled)
        printf("\n  %d unhandled exception(s)\n", g_unhandled);

    printf("\n%s\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return fails ? 1 : 0;
}
