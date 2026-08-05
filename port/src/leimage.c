/* Load a DOS/4GW LE executable into this process and rebase it.
 *
 * The original link-time bases (0x10000 / 0x140000) are unusable under Win32 --
 * the CRT heap and thread stacks already own that range before any user code
 * runs. That is fine: LE carries a complete internal fixup table precisely so
 * the extender can place objects anywhere, so we let the OS choose an address
 * and add the delta to every fixup target.
 *
 * Every object keeps its original distance from the first one, so a single
 * delta relocates the whole image and gp() stays a plain addition. The two
 * software-renderer binaries have two objects each; the 3dfx build has five,
 * with the code split across objects 1-4 and only object 5 writable.
 */

#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

le_image g_img;

#define PAGE_SZ  0x1000
#define MAX_OBJ  16

typedef struct {
    uint32_t virt_size, base, flags, page_index, page_count;
} le_object;

/* LE object flags */
#define OBJ_EXEC 0x0004

static uint32_t rd32(const uint8_t *p) { return *(const uint32_t *)p; }
static uint16_t rd16(const uint8_t *p) { return *(const uint16_t *)p; }

int le_load(const char *path, le_image *out)
{
    FILE *f = NULL;
    uint8_t *raw = NULL;
    long fsize;
    uint32_t le, num_pages, page_size, last_page, data_pages;
    uint32_t obj_tab, num_obj, page_map, fix_page_tab, fix_rec_tab;
    le_object obj[MAX_OBJ];
    uint32_t span, i, p, applied = 0, skipped = 0;
    uint32_t code_end = 0;
    uint8_t *mem = NULL;
    uint32_t *fpt;
    uint16_t host_ds;

    /* DOS/4GW would fill 16-bit selector fixups with the object's selector.
     * Under Win32 there is only one flat data selector and every displacement
     * in the image is a rebased absolute address, so writing the host's own DS
     * is what makes `mov ds, <fixup>` behave correctly. */
    __asm { mov ax, ds
            mov host_ds, ax }

    memset(out, 0, sizeof *out);

    f = fopen(path, "rb");
    if (!f) { LOGE("cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END); fsize = ftell(f); fseek(f, 0, SEEK_SET);
    raw = (uint8_t *)malloc(fsize);
    if (!raw || fread(raw, 1, fsize, f) != (size_t)fsize) {
        LOGE("cannot read %s", path); goto fail;
    }
    fclose(f); f = NULL;

    if (raw[0] != 'M' || raw[1] != 'Z') { LOGE("not an MZ file"); goto fail; }
    le = rd32(raw + 0x3c);
    if (le + 0xb0 > (uint32_t)fsize || raw[le] != 'L' || raw[le + 1] != 'E') {
        LOGE("no LE header at 0x%x", le); goto fail;
    }

    num_pages    = rd32(raw + le + 0x14);
    page_size    = rd32(raw + le + 0x28);
    last_page    = rd32(raw + le + 0x2c);
    obj_tab      = le + rd32(raw + le + 0x40);
    num_obj      = rd32(raw + le + 0x44);
    page_map     = le + rd32(raw + le + 0x48);
    fix_page_tab = le + rd32(raw + le + 0x68);
    fix_rec_tab  = le + rd32(raw + le + 0x6c);
    data_pages   = rd32(raw + le + 0x80);

    if (num_obj < 1 || num_obj > MAX_OBJ) {
        LOGE("unsupported object count %u", num_obj); goto fail;
    }

    for (i = 0; i < num_obj; i++) {
        const uint8_t *o = raw + obj_tab + i * 24;
        obj[i].virt_size  = rd32(o + 0);
        obj[i].base       = rd32(o + 4);
        obj[i].flags      = rd32(o + 8);
        obj[i].page_index = rd32(o + 12);
        obj[i].page_count = rd32(o + 16);
        LOGI("OBJ%u base=0x%08X size=0x%X flags=0x%X pages=%u",
             i + 1, obj[i].base, obj[i].virt_size, obj[i].flags,
             obj[i].page_count);
        if (obj[i].base < obj[0].base) {
            LOGE("OBJ%u lies below OBJ1; single-delta rebase would break",
                 i + 1);
            goto fail;
        }
        /* The executable extent is what gets marked PAGE_EXECUTE_READ and what
         * "is EIP in game code" is tested against. It is not always object 1:
         * the 3dfx build puts a 16-byte stub there and the real code in
         * object 2. */
        if ((obj[i].flags & OBJ_EXEC) &&
            obj[i].base + obj[i].virt_size > code_end)
            code_end = obj[i].base + obj[i].virt_size;
    }

    /* Keep every object at its original distance from the first. Nothing is
     * known to require it, but it costs nothing and defuses any accidental
     * assumption about the gaps between them. */
    span = (obj[num_obj - 1].base - obj[0].base) + obj[num_obj - 1].virt_size;
    mem = (uint8_t *)VirtualAlloc(NULL, span + IMAGE_HEAP_RESERVE,
                                  MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { LOGE("VirtualAlloc reserve(%u) failed: %lu",
                     span + IMAGE_HEAP_RESERVE, GetLastError());
                goto fail; }
    if (!VirtualAlloc(mem, span, MEM_COMMIT, PAGE_READWRITE)) {
        LOGE("VirtualAlloc commit(%u) failed: %lu", span, GetLastError());
        goto fail;
    }

    out->mem      = mem;
    out->size     = span;
    out->reserved = span + IMAGE_HEAP_RESERVE;
    out->heap_top = (uint32_t)(uintptr_t)(mem + span + IMAGE_HEAP_SIZE);
    out->delta = (uint32_t)(uintptr_t)mem - obj[0].base;
    out->img_lo    = obj[0].base;
    out->img_hi    = obj[num_obj - 1].base + obj[num_obj - 1].virt_size;
    out->code_size = code_end - obj[0].base;

    LOGI("mapped %u bytes at %p, reserved %u, delta = 0x%08X, break = 0x%08X",
         span, mem, out->reserved, out->delta, out->heap_top);
    LOGI("image 0x%08X..0x%08X, executable extent 0x%X",
         out->img_lo, out->img_hi, out->code_size);

    /* --- copy the page images ---------------------------------------- */
    for (i = 0; i < num_obj; i++) {
        uint8_t *dst = mem + (obj[i].base - obj[0].base);
        uint32_t off = 0, k;
        for (k = 0; k < obj[i].page_count; k++) {
            uint32_t idx = obj[i].page_index + k;      /* 1-based */
            const uint8_t *e = raw + page_map + (idx - 1) * 4;
            uint32_t page = ((uint32_t)e[0] << 16) | ((uint32_t)e[1] << 8) | e[2];
            uint32_t flags = e[3];
            uint32_t n = (page == num_pages) ? last_page : page_size;
            if (flags != 0) { LOGE("page %u flags=%u unsupported", idx, flags);
                              goto fail; }
            if (off >= obj[i].virt_size) break;        /* rest is BSS */
            if (off + n > obj[i].virt_size) n = obj[i].virt_size - off;
            memcpy(dst + off, raw + data_pages + (page - 1) * page_size, n);
            off += n;
        }
    }

    /* --- apply fixups, rebased --------------------------------------- */
    fpt = (uint32_t *)(raw + fix_page_tab);
    for (p = 0; p < num_pages; p++) {
        const uint8_t *r   = raw + fix_rec_tab + fpt[p];
        const uint8_t *end = raw + fix_rec_tab + fpt[p + 1];
        /* which object owns this page, and the page's offset within it */
        uint32_t oi = 0, pbase;
        uint8_t *objmem;

        for (i = 0; i < num_obj; i++) {
            if (obj[i].page_count &&
                p + 1 >= obj[i].page_index &&
                p + 1 <  obj[i].page_index + obj[i].page_count) { oi = i; break; }
        }
        if (i == num_obj) {                 /* page belongs to no object */
            if (r < end) skipped++;
            continue;
        }
        pbase  = (p + 1 - obj[oi].page_index) * PAGE_SZ;
        objmem = mem + (obj[oi].base - obj[0].base);

        while (r < end) {
            uint8_t src = r[0], flg = r[1];
            int count = 1, j;
            int16_t soff_one = 0;
            const uint8_t *list = NULL;
            uint32_t tobj, toff = 0, target;

            r += 2;
            if (src & 0x20) { count = *r++; }
            else            { soff_one = *(const int16_t *)r; r += 2; }

            if ((flg & 3) != 0) { LOGE("external fixup unsupported"); goto fail; }
            if (flg & 0x40) { tobj = rd16(r); r += 2; }
            else            { tobj = *r++; }
            if ((src & 0x0f) != 2) {
                if (flg & 0x10) { toff = rd32(r); r += 4; }
                else            { toff = rd16(r); r += 2; }
            }
            if (src & 0x20) { list = r; r += 2 * count; }

            if (tobj < 1 || tobj > num_obj) { skipped++; continue; }
            target = obj[tobj - 1].base + toff + out->delta;

            for (j = 0; j < count; j++) {
                int32_t soff = list ? *(const int16_t *)(list + 2 * j)
                                    : soff_one;
                int32_t at = (int32_t)pbase + soff;

                if ((src & 0x0f) == 2) {      /* 16-bit selector */
                    if (at < 0 || (uint32_t)at + 2 > obj[oi].virt_size) {
                        skipped++; continue;
                    }
                    *(uint16_t *)(objmem + at) = host_ds;
                    LOGI("selector fixup at orig 0x%08X <- 0x%04X",
                         obj[oi].base + at, host_ds);
                    applied++;
                    continue;
                }
                if ((src & 0x0f) != 7) { skipped++; continue; }
                /* A fixup may straddle a page boundary, which is exactly why
                 * we patch whole objects rather than page by page. */
                if (at < 0 || (uint32_t)at + 4 > obj[oi].virt_size) {
                    skipped++; continue;
                }
                *(uint32_t *)(objmem + at) = target;
                applied++;
            }
        }
    }

    out->entry     = rd32(raw + le + 0x1c) + obj[rd32(raw + le + 0x18) - 1].base
                     + out->delta;
    out->stack_top = rd32(raw + le + 0x24) + obj[rd32(raw + le + 0x20) - 1].base
                     + out->delta;

    LOGI("fixups applied=%u skipped=%u", applied, skipped);
    LOGI("entry = 0x%08X (orig 0x%08X)", out->entry, out->entry - out->delta);
    LOGI("stack = 0x%08X (orig 0x%08X)", out->stack_top,
         out->stack_top - out->delta);

    free(raw);
    return 1;

fail:
    if (f) fclose(f);
    if (raw) free(raw);
    if (mem) VirtualFree(mem, 0, MEM_RELEASE);
    memset(out, 0, sizeof *out);
    return 0;
}

void le_free(le_image *img)
{
    if (img->mem) VirtualFree(img->mem, 0, MEM_RELEASE);
    memset(img, 0, sizeof *img);
}
