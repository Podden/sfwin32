/* Miles-wide call tracer: --calltrace instruments every AIL_* entry point
 * ailtable.py's proof-based method can find in the running image -- not just
 * the 16 whose behaviour audio.c replaces -- and logs every call into any of
 * them to calltrace.csv, without altering behaviour at all.
 *
 * Built for one question: DESKTOP.EXE's cinematics play with no sound, and
 * int 66h (Miles' real-mode driver vector), direct port I/O, and a second
 * Miles-owned timer hook were all ruled out as the cause by direct
 * experiment. What is left is that cutscene audio, if it goes through Miles
 * at all, must call one of the library's other entry points -- the 16
 * hooked so far were recovered specifically for FIGHT.EXE's needs, not
 * DESKTOP's full surface. This answers that with runtime evidence instead
 * of more static guessing.
 *
 * Each hook is a detour, not a replacement: it logs (which function, who
 * called it), replays the function's own displaced prologue bytes, and falls
 * straight through into the unmodified original code. Where a function is
 * also one of audio.c's 16 real hooks, audio_install_hooks() runs after this
 * and overwrites the detour with its own -- harmless, since that function's
 * calls are already fully accounted for by audio.c's own logging.
 *
 * The prologue length is NOT a fixed 5 bytes. That was the first version's
 * bug, and it broke every single function: a compiler prologue like
 * `push ebx / push esi / push edi / push ebp / sub esp,4` (four one-byte
 * pushes then a three-byte sub) has no instruction boundary at byte 5, so
 * copying exactly 5 bytes there splits the sub in half and the replayed
 * "instruction" is garbage -- observed as DESKTOP.EXE hanging in an
 * unrelated segment-reload loop within seconds, since AIL_startup is one of
 * the first Miles calls made. The one place a fixed 5-byte copy was already
 * used (audio.c's voice_pool_thunk) had that manually verified for its one
 * specific function; it was never a general guarantee. The length per
 * function here is instead measured from the disassembly (scan.json) offline
 * -- the smallest number of bytes, from real instruction boundaries, that
 * reaches at least 5 -- so every replayed prologue is a whole number of real
 * instructions. */

#include "port.h"

#include <stdio.h>
#include <string.h>

typedef struct { uint32_t addr; int len; const char *name; } miles_fn;

/* Recovered by ailtable.py's proof-based method (each function named by the
 * AIL_ format string it pushes for its own debug log), extended to list
 * every match rather than only the 16 audio.c replaces. `len` is the safe
 * prologue length computed from scan.json's instruction boundaries. */
static const miles_fn g_miles_fight[] = {
    { 0x00042B40, 10, "AIL_API_read_INI" },
    { 0x00041C00, 10, "AIL_allocate_sample_handle" },
    { 0x0003DD70,  9, "AIL_branch_index" },
    { 0x00042F30,  9, "AIL_call_driver" },
    { 0x00042E80,  9, "AIL_delay" },
    { 0x00041400,  9, "AIL_end_sample" },
    { 0x0003F170,  9, "AIL_end_sequence" },
    { 0x00042180, 10, "AIL_get_IO_environment" },
    { 0x000432C0, 10, "AIL_get_real_vect" },
    { 0x00041980,  9, "AIL_init_sample" },
    { 0x00041EC0, 10, "AIL_install_DIG_INI" },
    { 0x00041DB0, 10, "AIL_install_DIG_driver_file" },
    { 0x00042070, 10, "AIL_install_driver" },
    { 0x0003D370, 10, "AIL_lock_channel" },
    { 0x0003D1E0,  9, "AIL_map_sequence_channel" },
    { 0x0003CB80,  9, "AIL_redbook_close" },
    { 0x0003CC30, 10, "AIL_redbook_open" },
    { 0x0003C310,  9, "AIL_redbook_play" },
    { 0x0003C920, 10, "AIL_redbook_status" },
    { 0x0003C210, 10, "AIL_redbook_stop" },
    { 0x0003C730,  9, "AIL_redbook_track_info" },
    { 0x00042A40, 10, "AIL_register_timer" },
    { 0x00042280,  8, "AIL_release_all_timers" },
    { 0x0003D2B0,  9, "AIL_release_channel" },
    { 0x00041A30,  9, "AIL_release_sample_handle" },
    { 0x0003F550,  9, "AIL_release_sequence_handle" },
    { 0x00042300,  9, "AIL_release_timer_handle" },
    { 0x00043080,  9, "AIL_restore_USE16_ISR" },
    { 0x00040F30, 10, "AIL_sample_status" },
    { 0x0003FAC0, 10, "AIL_sample_user_data" },
    { 0x0003DE30, 10, "AIL_sequence_position" },
    { 0x0003D530, 10, "AIL_sequence_user_data" },
    { 0x00040A70,  9, "AIL_set_digital_master_volume" },
    { 0x000434F0, 10, "AIL_set_driver_directory" },
    { 0x000433E0, 10, "AIL_set_preference" },
    { 0x00043200,  9, "AIL_set_real_vect" },
    { 0x00041790,  9, "AIL_set_sample_address" },
    { 0x00041100,  9, "AIL_set_sample_loop_count" },
    { 0x000411C0,  9, "AIL_set_sample_pan" },
    { 0x00041340,  9, "AIL_set_sample_playback_rate" },
    { 0x000416C0,  9, "AIL_set_sample_type" },
    { 0x00041280,  9, "AIL_set_sample_volume" },
    { 0x000427B0,  9, "AIL_set_timer_frequency" },
    { 0x00042870,  9, "AIL_set_timer_period" },
    { 0x00042930, 10, "AIL_set_timer_user" },
    { 0x000435F0, 11, "AIL_shutdown" },
    { 0x00041610,  9, "AIL_start_sample" },
    { 0x00042560,  9, "AIL_start_timer" },
    { 0x0011D4D0,  7, "AIL_startup" },
    { 0x0003F2D0,  9, "AIL_stop_sequence" },
    { 0x00042430,  9, "AIL_stop_timer" },
    { 0x00041D00,  9, "AIL_uninstall_DIG_driver" },
    { 0x00041FC0,  9, "AIL_uninstall_driver" },
};

static const miles_fn g_miles_desktop[] = {
    { 0x00047820, 10, "AIL_API_read_INI" },
    { 0x000468E0, 10, "AIL_allocate_sample_handle" },
    { 0x00042A50,  9, "AIL_branch_index" },
    { 0x00047C10,  9, "AIL_call_driver" },
    { 0x00047B60,  9, "AIL_delay" },
    { 0x000460E0,  9, "AIL_end_sample" },
    { 0x00043E50,  9, "AIL_end_sequence" },
    { 0x00046E60, 10, "AIL_get_IO_environment" },
    { 0x00047FA0, 10, "AIL_get_real_vect" },
    { 0x00046660,  9, "AIL_init_sample" },
    { 0x00046BA0, 10, "AIL_install_DIG_INI" },
    { 0x00046A90, 10, "AIL_install_DIG_driver_file" },
    { 0x00046D50, 10, "AIL_install_driver" },
    { 0x000450C0,  9, "AIL_load_sample_buffer" },
    { 0x00042050, 10, "AIL_lock_channel" },
    { 0x00041EC0,  9, "AIL_map_sequence_channel" },
    { 0x00047720, 10, "AIL_register_timer" },
    { 0x00046F60,  8, "AIL_release_all_timers" },
    { 0x00041F90,  9, "AIL_release_channel" },
    { 0x00046710,  9, "AIL_release_sample_handle" },
    { 0x00044230,  9, "AIL_release_sequence_handle" },
    { 0x00046FE0,  9, "AIL_release_timer_handle" },
    { 0x00047D60,  9, "AIL_restore_USE16_ISR" },
    { 0x000451B0, 10, "AIL_sample_buffer_ready" },
    { 0x00045C10, 10, "AIL_sample_status" },
    { 0x000447A0, 10, "AIL_sample_user_data" },
    { 0x00042B10, 10, "AIL_sequence_position" },
    { 0x00042210, 10, "AIL_sequence_user_data" },
    { 0x00045750,  9, "AIL_set_digital_master_volume" },
    { 0x000481D0, 10, "AIL_set_driver_directory" },
    { 0x000480C0, 10, "AIL_set_preference" },
    { 0x00047EE0,  9, "AIL_set_real_vect" },
    { 0x00046470,  9, "AIL_set_sample_address" },
    { 0x00045DE0,  9, "AIL_set_sample_loop_count" },
    { 0x00045EA0,  9, "AIL_set_sample_pan" },
    { 0x00046020,  9, "AIL_set_sample_playback_rate" },
    { 0x000463A0,  9, "AIL_set_sample_type" },
    { 0x00045F60,  9, "AIL_set_sample_volume" },
    { 0x00047490,  9, "AIL_set_timer_frequency" },
    { 0x00047550,  9, "AIL_set_timer_period" },
    { 0x00047610, 10, "AIL_set_timer_user" },
    { 0x000482D0, 11, "AIL_shutdown" },
    { 0x000462F0,  9, "AIL_start_sample" },
    { 0x00047240,  9, "AIL_start_timer" },
    { 0x000C4DC0,  7, "AIL_startup" },
    { 0x00043FB0,  9, "AIL_stop_sequence" },
    { 0x00047110,  9, "AIL_stop_timer" },
    { 0x000469E0,  9, "AIL_uninstall_DIG_driver" },
    { 0x00046CA0,  9, "AIL_uninstall_driver" },
};

/* Fixed logging overhead per trampoline, in bytes: pushfd(1) pushad(1)
 * mov eax,[esp+36](4) push eax(1) push imm32(5) call rel32(5) add esp,8(3)
 * popad(1) popfd(1) = 22, plus the final jmp rel32(5) back into the
 * original function = 27. The variable part is each function's own
 * prologue length. */
#define TRAMP_FIXED 27

#define MAX_HITS 500000
static struct { uint16_t idx; uint32_t caller; } g_hits[MAX_HITS];
static volatile LONG g_hit_count;

static const miles_fn *g_table;
static int              g_table_n;
static uint8_t         *g_tramp;

/* Called from inside each trampoline slot -- cdecl, so the two pushed
 * arguments are ours to read and the caller (the trampoline) cleans up. */
void __cdecl calltrace_hit(uint32_t idx, uint32_t caller_eip)
{
    LONG i = InterlockedIncrement(&g_hit_count) - 1;
    if (i < 0 || i >= MAX_HITS) return;
    g_hits[i].idx = (uint16_t)idx;
    g_hits[i].caller = caller_eip;
}

void calltrace_install(void)
{
    int i, total = 0;
    uint8_t *w;

    g_table   = g_desktop_mode ? g_miles_desktop : g_miles_fight;
    g_table_n = g_desktop_mode
                ? (int)(sizeof g_miles_desktop / sizeof g_miles_desktop[0])
                : (int)(sizeof g_miles_fight   / sizeof g_miles_fight[0]);

    for (i = 0; i < g_table_n; i++) total += TRAMP_FIXED + g_table[i].len;

    g_tramp = (uint8_t *)VirtualAlloc(NULL, (size_t)total,
                                      MEM_RESERVE | MEM_COMMIT,
                                      PAGE_EXECUTE_READWRITE);
    if (!g_tramp) { LOGE("calltrace: trampoline alloc failed"); return; }

    w = g_tramp;
    for (i = 0; i < g_table_n; i++) {
        uint8_t *p    = (uint8_t *)gp(g_table[i].addr);
        int      L    = g_table[i].len;
        uint8_t *slot = w;
        uint8_t  saved[16];       /* L is 7-11 in practice; margin for more */
        int32_t  rel;
        int      j;

        if (L < 5 || L > (int)sizeof saved) {
            LOGE("calltrace: %s has an out-of-range prologue length %d, "
                 "skipping it", g_table[i].name, L);
            continue;
        }
        memcpy(saved, p, (size_t)L);

        /* Preserve everything -- this must be invisible to the function and
         * its caller. Register/flag state at entry is exactly what it was
         * when the CALL that brought us here executed. */
        *w++ = 0x9C;                                  /* pushfd            */
        *w++ = 0x60;                                  /* pushad            */
        /* [esp+36] is the original return address: 4 bytes of pushfd plus
         * 32 bytes of pushad sit between it and the current esp. */
        *w++ = 0x8B; *w++ = 0x44; *w++ = 0x24; *w++ = 0x24; /* mov eax,[esp+36] */
        *w++ = 0x50;                                  /* push eax (caller) */
        *w++ = 0x68;                                  /* push imm32 (idx)  */
        *(uint32_t *)w = (uint32_t)i; w += 4;
        *w++ = 0xE8;                                  /* call calltrace_hit*/
        rel = (int32_t)((uint8_t *)(void *)calltrace_hit - (w + 4));
        *(int32_t *)w = rel; w += 4;
        *w++ = 0x83; *w++ = 0xC4; *w++ = 0x08;         /* add esp,8 (cdecl) */
        *w++ = 0x61;                                  /* popad             */
        *w++ = 0x9D;                                  /* popfd             */
        memcpy(w, saved, (size_t)L); w += L;           /* replayed prologue */
        *w++ = 0xE9;                                  /* jmp orig+L        */
        rel = (int32_t)((p + L) - (w + 4));
        *(int32_t *)w = rel; w += 4;

        if (w - slot != TRAMP_FIXED + L) {
            LOGE("calltrace: trampoline for %s is %d bytes, expected %d -- "
                 "aborting install", g_table[i].name, (int)(w - slot),
                 TRAMP_FIXED + L);
            return;
        }

        /* Detour the entry: a 5-byte jmp, NOP-padded out to the full safe
         * length so nothing can be left mid-instruction inside the span. */
        p[0] = 0xE9;
        *(int32_t *)(p + 1) = (int32_t)(slot - (p + 5));
        for (j = 5; j < L; j++) p[j] = 0x90;
    }
    LOGI("calltrace: %d Miles entry points instrumented (%s table), "
         "%d bytes of trampoline", g_table_n,
         g_desktop_mode ? "desktop" : "fight", total);
}

void calltrace_dump(void)
{
    FILE *f;
    LONG n, i;

    if (!g_table) return;
    n = g_hit_count;
    if (n > MAX_HITS) {
        LOGW("calltrace: %ld calls recorded, log capped at %d -- ring buffer "
             "would be needed for a full trace of a longer run", (long)n,
             MAX_HITS);
        n = MAX_HITS;
    }
    if (!n) { LOGI("calltrace: no instrumented function was ever called"); return; }

    f = fopen("calltrace.csv", "w");
    if (!f) { LOGE("calltrace: could not open calltrace.csv for writing"); return; }
    fprintf(f, "seq,function,caller_orig\n");
    for (i = 0; i < n; i++)
        fprintf(f, "%ld,%s,0x%08X\n", (long)i, g_table[g_hits[i].idx].name,
                (uint32_t)(g_hits[i].caller - g_img.delta));
    fclose(f);
    LOGI("calltrace: wrote %ld call(s) to calltrace.csv", (long)n);
}
