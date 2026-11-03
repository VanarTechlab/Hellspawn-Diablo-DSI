#define _GNU_SOURCE
/* Capa SDL 1.2 minima para DSi - etapa 1: video 8 bpp 256x192, reloj, errores.
   Lo no implementado devuelve error y lo deja en SDL_GetError(). */
#include <nds.h>
#include <SDL/SDL.h>
#include <nds/cothread.h>
#include <threads.h>
#include <malloc.h>

/* 1 = vigilantes de depuracion activos, 0 = rapido */
#ifndef NDS_DEBUG
#define NDS_DEBUG 0
#endif

static volatile int guard_bad, guard_w, guard_h;
static volatile unsigned guard_pre, guard_post;
static volatile int heap_halt;
static void hb_scan(void);
static void guard_check(void);

typedef struct lua_State lua_State;
extern int lua_getglobal(lua_State *L, const char *name);
extern void lua_settop(lua_State *L, int idx);
void *nds_lua_state;
static volatile int probe_dead, dbl_free, dbl_w, dbl_h;
static volatile unsigned probe_ok_lr, probe_ok_code, probe_cur_lr, probe_cur_code;
static void nds_lua_light(unsigned code, unsigned lr);
static char probe_msg1[40], probe_msg2[40], probe_msg3[40], probe_prev[40];
static unsigned char *reg_t;
static lua_State *reg_L;
static volatile unsigned reg_checks, rd_cnt;
static unsigned reg_G, reg_goff, reg_roff;
static unsigned reg_check_lg(lua_State *L);
static void reg_init(lua_State *L);
static unsigned reg_check(void);
static void reg_fatal(unsigned bad, unsigned code, unsigned lr);
static void nds_lua_probe(const char *what);
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NDS-ZOOM: tamano del mundo que dibuja el motor (se reduce a 256x192).
   256x192 = sin reducir; 288x216 = un poco mas de vista; 320x240 = bastante mas */
#define WORLD_W 288
#define WORLD_H 216
#define LCD_W 256
#define SCREEN_W WORLD_W
/* pantalla logica de 256x320: filas 0-191 en la tactil (mundo), 192-319 en la de arriba (panel) */
#define SCREEN_H (WORLD_H + 128)
#define LCD_H 192
#define TOP_ROWS (SCREEN_H - LCD_H)
#define SCREEN_BYTES (SCREEN_W * SCREEN_H)
static Uint8 pal_lum[256]; /* NDS-SHARP: declarada antes de SDL_SetPalette */
void nds_audio_update(void); /* NDS-AUDIO */

/* ---------------- errores ---------------- */
static char err_buf[160];

void SDL_SetError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, sizeof(err_buf), fmt, ap);
    va_end(ap);
}
char *SDL_GetError(void) { return err_buf; }
void SDL_ClearError(void) { err_buf[0] = 0; }
void SDL_Error(SDL_errorcode code) { SDL_SetError("SDL_Error %d", (int)code); }

int SDL_snprintf(char *text, size_t maxlen, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(text, maxlen, fmt, ap);
    va_end(ap);
    return n;
}

/* ---------------- reloj (timers 0 y 1 en cascada, ~1 kHz) ---------------- */
static int timer_ready;
static u16 tick_last;
static Uint32 tick_hi;

static void timer_init(void)
{
    TIMER_DATA(1) = 0;
    TIMER_CR(1) = TIMER_ENABLE | TIMER_CASCADE;
    TIMER_DATA(0) = TIMER_FREQ_64(1000);
    TIMER_CR(0) = TIMER_ENABLE | TIMER_DIV_64;
    timer_ready = 1;
}

/* NDS-RAIZ: raiz de los datos: "sd:" en DSi/3DS, "fat:" en melonDS con DLDI */
#include <dirent.h>
#include <string.h>
#include <stdio.h>
char nds_root[8] = "sd:";
static const char *nds_r(const char *rest)
{
    static char buf[8][128];
    static int i;
    char *b = buf[i++ & 7];
    snprintf(b, 128, "%s%s", nds_root, rest);
    return b;
}

/* NDS-OPCIONES: pantallas intercambiadas (1 = juego arriba). Se guarda en saves/pantallas.txt */
static int nds_swap = -1;
int nds_get_swap(void)
{
    if (nds_swap < 0) {
        FILE *f = fopen(nds_r("/devilutionx/saves/pantallas.txt"), "r");
        nds_swap = 0;
        if (f) {
            nds_swap = fgetc(f) == '1';
            fclose(f);
        }
    }
    return nds_swap;
}
static void nds_apply_swap(void)
{
    if (nds_get_swap())
        lcdMainOnTop();
    else
        lcdMainOnBottom();
}
void nds_set_swap(int on)
{
    FILE *f;
    nds_swap = on ? 1 : 0;
    nds_apply_swap();
    f = fopen(nds_r("/devilutionx/saves/pantallas.txt"), "w");
    if (f) {
        fputc(nds_swap ? '1' : '0', f);
        fclose(f);
    }
}

/* ---- latido de depuracion: hilo propio que escribe contadores cada segundo ---- */
static volatile int no_page;
static volatile unsigned hb_flip, hb_poll, hb_delay, hb_ticks, hb_mtx, hb_sec;
static const char *volatile hb_last = "-";
static volatile int hb_loader = -1;

static int hb_thread(void *arg)
{
    u16 prev = TIMER_DATA(1);
    (void)arg;
    for (;;) {
        u16 t = TIMER_DATA(1);
        if ((u16)(t - prev) >= 1000) {
            prev = t;
            hb_sec++;
            no_page++;
            {
                static unsigned last_flip;
                printf("\x1b[21;0Hs%u f%u fps%u p%u d%u   ", hb_sec, hb_flip, hb_flip - last_flip, hb_poll, hb_delay);
                last_flip = hb_flip;
            }
            if (guard_bad)
                printf("\x1b[18;0HGUARD %dx%d a%u d%u  ", guard_w, guard_h, guard_pre, guard_post);
            if (probe_dead)
                printf("\x1b[14;0H%s   \x1b[15;0H%s   \x1b[16;0H%s   ", probe_msg1, probe_msg2, probe_msg3);
            if (dbl_free)
                printf("\x1b[17;0HDOBLE FREE %dx%d   ", dbl_w, dbl_h);
            printf("\x1b[22;0Ht%u m%u L%d %s        ", hb_ticks, hb_mtx,
                hb_loader >= 0 ? (int)cothread_has_joined(hb_loader) : -1, (const char *)hb_last);
            no_page--;
        }
        thrd_yield();
    }
    return 0;
}

static void hb_start(void)
{
    if (!timer_ready)
        timer_init();
    cothread_create(hb_thread, NULL, 16 * 1024, COTHREAD_DETACHED);
}

static volatile unsigned trace_cnt;
static void nds_lua_light(unsigned code, unsigned lr)
{
    if (!NDS_DEBUG)
        return;
    static int lseen;
    lua_State *L = (lua_State *)nds_lua_state;
    int g, p;
    if (!L) {
        reg_t = NULL;
        return;
    }
    if (probe_dead)
        return;
    if (L != reg_L)
        reg_t = NULL;
    probe_cur_lr = lr;
    probe_cur_code = code;
    if (reg_t) {
        unsigned badlg = reg_check_lg(L);
        if (badlg)
            reg_fatal(badlg, code, lr);
    }
    g = lua_getglobal(L, "_G");
    lua_settop(L, -2);
    p = lua_getglobal(L, "pairs");
    lua_settop(L, -2);
    if (g == 5 && p == 6) {
        unsigned bad;
        if (!reg_t)
            reg_init(L);
        bad = reg_t ? reg_check() : 0u;
        if (bad)
            reg_fatal(bad, code, lr);
        lseen = 1;
        probe_ok_lr = lr;
        probe_ok_code = code;
    } else if (lseen) {
        probe_dead = 1;
        snprintf(probe_msg1, sizeof probe_msg1, "ROTO(luz) _G=%d pairs=%d", g, p);
        snprintf(probe_msg2, sizeof probe_msg2, "bien: cod%u lr%08X", probe_ok_code, probe_ok_lr);
        snprintf(probe_msg3, sizeof probe_msg3, "mal:  cod%u lr%08X", code, lr);
    }
}

static void nds_lua_probe(const char *what)
{
    if (!NDS_DEBUG)
        return;
    static int seen, calls, nscan;
    if ((++nscan & 7) == 0)
        hb_scan();
    lua_State *L = (lua_State *)nds_lua_state;
    size_t n;
    const char *t22, *t25;
    if (!what)
        what = "";
    n = strlen(what);
    t22 = n > 22 ? what + (n - 22) : what;
    t25 = n > 25 ? what + (n - 25) : what;
    if (L && !probe_dead) {
        int g, p;
        g = lua_getglobal(L, "_G");
        lua_settop(L, -2);
        p = lua_getglobal(L, "pairs");
        lua_settop(L, -2);
        calls++;
        if (g == 5 && p == 6) {
            seen = 1;
        } else if (seen || calls > 200) {
            probe_dead = 1;
            snprintf(probe_msg1, sizeof probe_msg1, "ROTO _G=%d pairs=%d vistos=%d", g, p, seen);
            snprintf(probe_msg2, sizeof probe_msg2, "bien tras:%s", probe_prev);
            snprintf(probe_msg3, sizeof probe_msg3, "mal en:%s", t25);
        }
    }
    if (!probe_dead) {
        strncpy(probe_prev, t22, sizeof probe_prev - 1);
        probe_prev[sizeof probe_prev - 1] = 0;
    }
}

void nds_trace_open(const char *path)
{
    nds_lua_light(7, (unsigned)__builtin_return_address(0));
    nds_lua_probe(path);
    size_t n = strlen(path);
    const char *tail = (n > 31) ? path + (n - 31) : path;
    trace_cnt++;
    no_page++;
    printf("\x1b[19;0H%-31s", tail);
    printf("\x1b[20;0Hopens=%u        ", trace_cnt);
    no_page--;
    thrd_yield();
}

void abort(void)
{
    unsigned long sp;
    unsigned long *q;
    int i, shown = 0;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    no_page++;
    printf("\x1b[2J\x1b[0;0HABORT! lr=%08lX hilo=%d\n", (unsigned long)__builtin_return_address(0), (int)thrd_current());
    q = (unsigned long *)sp;
    for (i = 0; i < 1024 && shown < 12; i++) {
        unsigned long v = q[i];
        if (v >= 0x02000000UL && v < 0x02200000UL && (v & 1UL)) {
            printf("%08lX ", v);
            if ((++shown % 3) == 0)
                printf("\n");
        }
    }
    for (;;)
        swiWaitForVBlank();
}

static volatile unsigned vbl_count, hb_seen_sec, hb_seen_vbl;

__attribute__((target("arm"), noinline))
static unsigned long read_sp_irq(void)
{
    unsigned long cpsr, sp;
    __asm__ volatile(
        "mrs %0, cpsr\n\t"
        "msr cpsr_c, #0xD2\n\t"
        "mov %1, sp\n\t"
        "msr cpsr_c, %0\n\t"
        : "=&r"(cpsr), "=&r"(sp)
        :
        : "memory");
    return sp;
}

static void irq_hex(char *o, unsigned long v)
{
    static const char d[] = "0123456789ABCDEF";
    int i;
    for (i = 7; i >= 0; i--) {
        o[i] = d[v & 15];
        v >>= 4;
    }
    o[8] = 0;
}

static void irq_dec(char *o, unsigned v)
{
    char tmp[12];
    int n = 0, i;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < 11);
    for (i = 0; i < n; i++)
        o[i] = tmp[n - 1 - i];
    o[n] = 0;
}

static void irq_puts(int x, int y, const char *t)
{
    consoleSetCursor(consoleGetDefault(), x, y);
    while (*t)
        consolePrintChar(*t++);
}

/* NDS-RAPIDO4: pagina de VRAM pendiente de mostrar (0 o 1), -1 = nada */
static volatile int vpage_show = -1;

static void nds_vblank(void)
{
    if (vpage_show >= 0) { /* NDS-RAPIDO4: cambiar de pagina fuera de la zona visible */
        REG_BG3CNT = (u16)((REG_BG3CNT & ~(31 << 8)) | ((vpage_show ? 4 : 0) << 8));
        vpage_show = -1;
    }
    if (heap_halt)
        return;
    vbl_count++;
    if (!NDS_DEBUG) /* NDS-TECLADO2: el vigilante de cuelgues pintaba una raya en el logo */
        return;
    if (hb_sec != hb_seen_sec) {
        hb_seen_sec = hb_sec;
        hb_seen_vbl = vbl_count;
        return;
    }
    if (vbl_count - hb_seen_vbl >= 180 && ((vbl_count - hb_seen_vbl) % 300) == 0) {
        unsigned long *p = (unsigned long *)read_sp_irq();
        char line[40];
        char num[12];
        int r, k;
        irq_dec(num, hb_sec);
        irq_puts(0, 13, "COLGADO s");
        irq_puts(9, 13, num);
        irq_puts(14, 13, " T=");
        irq_hex(line, (unsigned long)&nds_trace_open);
        irq_puts(17, 13, line);
        for (r = 0; r < 4; r++) {
            for (k = 0; k < 3; k++) {
                irq_hex(line, p[r * 3 + k]);
                irq_puts(k * 9, 14 + r, line);
            }
        }
    }
}

#include <nds/exceptions.h>

static void exc_puts(PrintConsole *pc, int x, int y, const char *t)
{
    consoleSetCursor(pc, x, y);
    while (*t)
        consolePrintChar(*t++);
}

static void nds_exception(void)
{
    PrintConsole *pc;
    char b[12];
    unsigned long *sp;
    int i, r, k;

    pc = consoleDemoInit();
    BG_PALETTE_SUB[0] = RGB15(0, 0, 12);
    exc_puts(pc, 0, 0, "EXCEPCION r0..r15");
    exc_puts(pc, 0, 14, "ok:");
    exc_puts(pc, 3, 14, probe_prev);
    if (probe_dead) {
        exc_puts(pc, 0, 15, probe_msg1);
        exc_puts(pc, 0, 16, probe_msg2);
        exc_puts(pc, 0, 17, probe_msg3);
    }
    if (dbl_free) {
        exc_puts(pc, 0, 18, "DOBLE FREE");
        irq_hex(b, (unsigned long)dbl_w);
        exc_puts(pc, 11, 18, b);
        irq_hex(b, (unsigned long)dbl_h);
        exc_puts(pc, 20, 18, b);
    }
    exc_puts(pc, 0, 20, "okLR");
    irq_hex(b, (unsigned long)probe_ok_lr);
    exc_puts(pc, 5, 20, b);
    irq_dec(b, probe_ok_code);
    exc_puts(pc, 15, 20, b);
    exc_puts(pc, 0, 21, "curLR");
    irq_hex(b, (unsigned long)probe_cur_lr);
    exc_puts(pc, 6, 21, b);
    irq_dec(b, probe_cur_code);
    exc_puts(pc, 16, 21, b);
    if (!probe_dead) {
        unsigned lv = (unsigned)nds_lua_state;
        exc_puts(pc, 0, 15, "L");
        irq_hex(b, lv);
        exc_puts(pc, 2, 15, b);
        exc_puts(pc, 11, 15, "G");
        irq_hex(b, reg_G);
        exc_puts(pc, 13, 15, b);
        irq_dec(b, reg_goff);
        exc_puts(pc, 23, 15, b);
        irq_dec(b, reg_roff);
        exc_puts(pc, 27, 15, b);
        if (reg_G && lv >= 0x02000000u && lv < 0x03000000u && (lv & 3u) == 0) {
            const unsigned char *r = (const unsigned char *)reg_G + reg_roff;
            exc_puts(pc, 0, 16, "L.G");
            irq_hex(b, *(const unsigned *)(lv + reg_goff));
            exc_puts(pc, 4, 16, b);
            exc_puts(pc, 0, 17, "G.reg");
            irq_hex(b, *(const unsigned *)r);
            exc_puts(pc, 6, 17, b);
            irq_hex(b, r[4]);
            exc_puts(pc, 16, 17, b + 6);
        }
    }
    exc_puts(pc, 0, 22, "reg");
    irq_hex(b, (unsigned long)reg_t);
    exc_puts(pc, 4, 22, b);
    irq_dec(b, reg_checks);
    exc_puts(pc, 13, 22, b);
    exc_puts(pc, 0, 23, "fread");
    irq_dec(b, rd_cnt);
    exc_puts(pc, 6, 23, b);
    if (guard_bad) {
        exc_puts(pc, 0, 19, "GUARD");
        irq_hex(b, (unsigned long)guard_pre);
        exc_puts(pc, 6, 19, b);
        irq_hex(b, (unsigned long)guard_post);
        exc_puts(pc, 15, 19, b);
    }
    for (i = 0; i < 16; i++) {
        irq_hex(b, (unsigned long)exceptionRegisters[i]);
        exc_puts(pc, (i % 3) * 9, 1 + i / 3, b);
    }
    sp = (unsigned long *)exceptionRegisters[13];
    if (((unsigned long)sp & 3) == 0 && (unsigned long)sp >= 0x02000000UL
        && (unsigned long)sp < 0x02FFFF00UL) {
        exc_puts(pc, 0, 8, "pila:");
        for (r = 0; r < 5; r++) {
            for (k = 0; k < 3; k++) {
                irq_hex(b, sp[r * 3 + k]);
                exc_puts(pc, k * 9, 9 + r, b);
            }
        }
    }
    for (;;) {
    }
}

/* NDS-MEMTRK-FIX: funciones viejas renombradas */
/* ---------- depurador de monton (requiere -Wl,--wrap=malloc/free/calloc/realloc) ---------- */
extern void *__real_malloc(size_t n);
extern void __real_free(void *p);
extern void *__real_realloc(void *p, size_t n);

#define HB_MAGIC1 0xB10CC0DEu
#define HB_MAGIC2 0xC0DEB10Cu
#define HB_RZ 16u
#define HB_QMAX_BYTES (2u * 1024u * 1024u)
#define HB_QMAX_N 5000u

typedef struct HBlk {
    unsigned magic1, size, base, state;
    unsigned alloc_lr, a2, a3, seq;
    unsigned free_lr, f2, f3, magic2;
    struct HBlk *prev, *next;
    unsigned pad0, pad1;
} HBlk;
typedef char hb_size_check[(sizeof(HBlk) == 64) ? 1 : -1];

static HBlk *hb_live, *hb_qhead, *hb_qtail;
static unsigned hb_qbytes, hb_qn, hb_seq, hb_ops;
static int hb_busy;

static int hb_ptr_ok(const void *p)
{
    unsigned v = (unsigned)p;
    return v >= 0x02000000u && v < 0x03000000u && (v & 3u) == 0;
}

static void hb_row(PrintConsole *pc, int y, const char *lab, unsigned a, unsigned b, unsigned c, int n)
{
    char t[12];
    exc_puts(pc, 0, y, lab);
    irq_hex(t, a);
    exc_puts(pc, 3, y, t);
    if (n > 1) {
        irq_hex(t, b);
        exc_puts(pc, 12, y, t);
    }
    if (n > 2) {
        irq_hex(t, c);
        exc_puts(pc, 21, y, t);
    }
}

static void hb_fatal(const char *kind, HBlk *b, const unsigned char *at)
{
    PrintConsole *pc;
    char t[12];
    unsigned a;
    int i;
    heap_halt = 1;
    pc = consoleDemoInit();
    BG_PALETTE_SUB[0] = RGB15(10, 0, 10);
    exc_puts(pc, 0, 0, "HEAP CORRUPTO:");
    exc_puts(pc, 15, 0, kind);
    if (hb_ptr_ok(b)) {
        hb_row(pc, 2, "b:", (unsigned)b, b->magic1, b->magic2, 3);
        hb_row(pc, 3, "t:", b->size, b->state, b->seq, 3);
        hb_row(pc, 4, "a:", b->alloc_lr, b->a2, b->a3, 3);
        hb_row(pc, 5, "f:", b->free_lr, b->f2, b->f3, 3);
    } else {
        hb_row(pc, 2, "b:", (unsigned)b, 0, 0, 1);
    }
    a = (unsigned)at & ~3u;
    if (at && a >= 0x02000000u && a < 0x02FFFF00u) {
        hb_row(pc, 7, "en", a, hb_ptr_ok(b) ? (unsigned)(at - (const unsigned char *)(b + 1)) : 0u, 0, 2);
        for (i = 0; i < 4; i++) {
            irq_hex(t, ((const unsigned *)a)[i]);
            exc_puts(pc, (i & 1) * 9, 8 + (i >> 1), t);
        }
    }
    exc_puts(pc, 0, 11, "tras:");
    exc_puts(pc, 6, 11, probe_prev);
    hb_row(pc, 12, "op", hb_ops, hb_qn, hb_qbytes, 3);
    for (;;)
        swiWaitForVBlank();
}

static void hb_callers(unsigned lr, unsigned *c2, unsigned *c3)
{
    unsigned sp, i, found = 0;
    const unsigned *w;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    w = (const unsigned *)sp;
    *c2 = 0;
    *c3 = 0;
    for (i = 0; i < 96 && found < 2; i++) {
        unsigned v = w[i];
        if ((v & 1u) && v >= 0x02000000u && v < 0x02200000u && v != lr) {
            if (!found)
                *c2 = v;
            else
                *c3 = v;
            found++;
        }
    }
}

static unsigned hb_cost(const HBlk *b)
{
    return ((b->size + 3u) & ~3u) + (unsigned)sizeof(HBlk) + HB_RZ;
}

static void hb_check_live(HBlk *b)
{
    const unsigned char *user = (const unsigned char *)(b + 1);
    unsigned n4, i;
    if (b->magic1 != HB_MAGIC1 || b->magic2 != HB_MAGIC2 || b->state != 1)
        hb_fatal("HDR", b, 0);
    n4 = (b->size + 3u) & ~3u;
    for (i = 0; i < HB_RZ; i++)
        if (user[n4 + i] != 0xA5)
            hb_fatal("POST", b, user + n4 + i);
}

static void hb_check_freed(HBlk *b)
{
    const unsigned *w = (const unsigned *)(b + 1);
    const unsigned char *user = (const unsigned char *)(b + 1);
    unsigned n4, i;
    if (b->magic1 != HB_MAGIC1 || b->magic2 != HB_MAGIC2 || b->state != 2)
        hb_fatal("HDRLIB", b, 0);
    n4 = (b->size + 3u) & ~3u;
    for (i = 0; i < n4 / 4; i++)
        if (w[i] != 0xDDDDDDDDu)
            hb_fatal("UAF", b, (const unsigned char *)&w[i]);
    for (i = 0; i < HB_RZ; i++)
        if (user[n4 + i] != 0xA5)
            hb_fatal("POSTLIB", b, user + n4 + i);
}

static void hb_evict(void)
{
    HBlk *b = hb_qhead;
    if (!b)
        return;
    hb_check_freed(b);
    hb_qhead = b->next;
    if (!hb_qhead)
        hb_qtail = NULL;
    hb_qbytes -= hb_cost(b);
    hb_qn--;
    __real_free((void *)b->base);
}

static void hb_scan(void)
{
    if (!NDS_DEBUG)
        return;
    HBlk *b, *last = NULL;
    unsigned n = 0;
    if (hb_busy)
        return;
    hb_busy = 1;
    for (b = hb_live; b; b = b->next) {
        if (!hb_ptr_ok(b))
            hb_fatal("LISTA", last, (const unsigned char *)b);
        hb_check_live(b);
        last = b;
        if (++n > 500000u)
            break;
    }
    for (b = hb_qhead; b; b = b->next) {
        if (!hb_ptr_ok(b))
            hb_fatal("LISTAQ", last, (const unsigned char *)b);
        hb_check_freed(b);
        last = b;
    }
    hb_busy = 0;
}

static void *hb_alloc(size_t n, unsigned lr)
{
    if (!NDS_DEBUG)
        return __real_malloc(n);
    unsigned n4;
    unsigned char *base, *user;
    HBlk *b;
    if (n > 0x01000000u)
        return NULL;
    if ((++hb_ops & 1023u) == 0)
        hb_scan();
    n4 = ((unsigned)n + 3u) & ~3u;
    base = (unsigned char *)__real_malloc(sizeof(HBlk) + n4 + HB_RZ);
    while (!base && hb_qn) {
        hb_evict();
        base = (unsigned char *)__real_malloc(sizeof(HBlk) + n4 + HB_RZ);
    }
    if (!base)
        return NULL;
    b = (HBlk *)base;
    user = base + sizeof(HBlk);
    b->magic1 = HB_MAGIC1;
    b->magic2 = HB_MAGIC2;
    b->size = (unsigned)n;
    b->base = (unsigned)base;
    b->state = 1;
    b->alloc_lr = lr;
    hb_callers(lr, &b->a2, &b->a3);
    b->seq = ++hb_seq;
    b->free_lr = 0;
    b->f2 = 0;
    b->f3 = 0;
    b->pad0 = 0;
    b->pad1 = 0;
    b->prev = NULL;
    b->next = hb_live;
    if (hb_live)
        hb_live->prev = b;
    hb_live = b;
    memset(user + n, 0xA5, (n4 - (unsigned)n) + HB_RZ);
    return user;
}

static void hb_release(HBlk *b, unsigned lr)
{
    unsigned n4 = (b->size + 3u) & ~3u;
    if ((++hb_ops & 1023u) == 0)
        hb_scan();
    if ((b->prev && !hb_ptr_ok(b->prev)) || (b->next && !hb_ptr_ok(b->next)))
        hb_fatal("LISTA", b, 0);
    if (b->prev)
        b->prev->next = b->next;
    else
        hb_live = b->next;
    if (b->next)
        b->next->prev = b->prev;
    memset(b + 1, 0xDD, n4);
    b->free_lr = lr;
    hb_callers(lr, &b->f2, &b->f3);
    b->state = 2;
    b->prev = NULL;
    b->next = NULL;
    if (hb_qtail)
        hb_qtail->next = b;
    else
        hb_qhead = b;
    hb_qtail = b;
    hb_qn++;
    hb_qbytes += hb_cost(b);
    while (hb_qhead && (hb_qn > HB_QMAX_N || hb_qbytes > HB_QMAX_BYTES))
        hb_evict();
}

void *hb_old_wrap_malloc(size_t n)
{
    return hb_alloc(n, (unsigned)__builtin_return_address(0));
}

void *hb_old_wrap_calloc(size_t a, size_t n)
{
    void *p;
    size_t t;
    if (a != 0 && n > 0x01000000u / a)
        return NULL;
    t = a * n;
    p = hb_alloc(t, (unsigned)__builtin_return_address(0));
    if (p)
        memset(p, 0, t);
    return p;
}

void hb_old_wrap_free(void *p)
{
    HBlk *b;
    unsigned lr = (unsigned)__builtin_return_address(0);
    if (!p)
        return;
    b = (HBlk *)((unsigned char *)p - sizeof(HBlk));
    if (!hb_ptr_ok(b) || b->magic1 != HB_MAGIC1 || b->magic2 != HB_MAGIC2) {
        __real_free(p);
        return;
    }
    if (b->state == 2)
        hb_fatal("DFREE", b, 0);
    hb_check_live(b);
    hb_release(b, lr);
}

void *hb_old_wrap_realloc(void *p, size_t n)
{
    HBlk *b;
    void *q;
    size_t m;
    unsigned lr = (unsigned)__builtin_return_address(0);
    if (!p)
        return hb_alloc(n, lr);
    b = (HBlk *)((unsigned char *)p - sizeof(HBlk));
    if (!hb_ptr_ok(b) || b->magic1 != HB_MAGIC1 || b->magic2 != HB_MAGIC2)
        return __real_realloc(p, n);
    if (n == 0) {
        hb_old_wrap_free(p);
        return NULL;
    }
    if (b->state == 2)
        hb_fatal("DFREE", b, 0);
    hb_check_live(b);
    q = hb_alloc(n, lr);
    if (!q)
        return NULL;
    m = b->size < n ? b->size : n;
    memcpy(q, p, m);
    hb_release(b, lr);
    return q;
}


/* ---------- vigilante del registro de Lua (Lua 5.4, LUA_32BITS) ---------- */
static Uint8 *video_pixels;
extern unsigned short nds_gnW __asm__("_ZN10devilution13gnScreenWidthE") __attribute__((weak));
extern unsigned short nds_gnH __asm__("_ZN10devilution14gnScreenHeightE") __attribute__((weak));
extern unsigned short nds_gnV __asm__("_ZN10devilution16gnViewportHeightE") __attribute__((weak));
extern void lua_pushvalue(lua_State *L, int idx);
extern const void *lua_topointer(lua_State *L, int idx);
extern void lua_pushnil(lua_State *L);
extern size_t __real_fread(void *ptr, size_t size, size_t n, FILE *f);

static void reg_init(lua_State *L)
{
    lua_pushvalue(L, -1001000); /* LUA_REGISTRYINDEX (0xFFF0B9D8 en el binario) */
    reg_t = (unsigned char *)lua_topointer(L, -1);
    lua_settop(L, -2);
    /* borra la copia del registro que queda en la pila (falso positivo) */
    lua_pushnil(L);
    lua_settop(L, -2);
    reg_L = L;
    if (!hb_ptr_ok(reg_t))
        reg_t = NULL;
    reg_G = 0;
    if (reg_t) {
        const unsigned *w = (const unsigned *)L;
        unsigned i, j;
        for (i = 3; i < 12 && !reg_G; i++) {
            const unsigned *gw = (const unsigned *)w[i];
            if (!hb_ptr_ok(gw))
                continue;
            for (j = 0; j < 32; j++) {
                if (gw[j] == (unsigned)reg_t && (gw[j + 1] & 0xFFu) == 0x45u) {
                    reg_G = (unsigned)gw;
                    reg_goff = i * 4u;
                    reg_roff = j * 4u;
                    break;
                }
            }
        }
    }
}

/* comprueba L->l_G y G->l_registry (lo que usa lua_rawgeti con LUA_REGISTRYINDEX) */
static unsigned reg_check_lg(lua_State *L)
{
    const unsigned char *r;
    if (!reg_G)
        return 0;
    if (*(const unsigned *)((const unsigned char *)L + reg_goff) != reg_G)
        return (unsigned)L + reg_goff;
    r = (const unsigned char *)reg_G + reg_roff;
    if (*(const unsigned *)r != (unsigned)reg_t || r[4] != 0x45u)
        return (unsigned)r;
    return 0;
}

static int reg_tag_ok(unsigned t, unsigned v)
{
    if (t >= 0x80u || (t & 0x0Fu) > 11u)
        return 0;
    if ((t & 0x40u) && !hb_ptr_ok((const void *)v))
        return 0;
    return 1;
}

/* Table: +7 lsizenode, +8 alimit, +12 array (TValue 8 B), +16 node (Node 16 B) */
static unsigned reg_check(void)
{
    const unsigned char *t = reg_t;
    unsigned alimit = *(const unsigned *)(t + 8);
    unsigned arr = *(const unsigned *)(t + 12);
    unsigned node = *(const unsigned *)(t + 16);
    unsigned lsn = t[7], nsz, i;
    reg_checks++;
    if (alimit > 65536u || lsn > 16u || (alimit && !hb_ptr_ok((const void *)arr)) || !hb_ptr_ok((const void *)node))
        return (unsigned)t;
    for (i = 0; i < alimit; i++) {
        const unsigned char *e = (const unsigned char *)arr + i * 8u;
        if (!reg_tag_ok(e[4], *(const unsigned *)e))
            return (unsigned)e;
    }
    nsz = 1u << lsn;
    for (i = 0; i < nsz; i++) {
        const unsigned char *n = (const unsigned char *)node + i * 16u;
        int nx = *(const int *)(n + 8);
        if (!reg_tag_ok(n[4], *(const unsigned *)n) || !reg_tag_ok(n[5], *(const unsigned *)(n + 12))
            || (int)i + nx < 0 || (unsigned)((int)i + nx) >= nsz)
            return (unsigned)n;
    }
    return 0;
}

static HBlk *reg_owner(unsigned a, HBlk *list, int *found)
{
    HBlk *b;
    unsigned n = 0;
    for (b = list; b && hb_ptr_ok(b) && n < 500000u; b = b->next, n++) {
        unsigned lo = (unsigned)b, hi = (unsigned)(b + 1) + ((b->size + 3u) & ~3u) + HB_RZ;
        if (a >= lo && a < hi) {
            *found = 1;
            return b;
        }
    }
    *found = 0;
    return NULL;
}

static void reg_fatal(unsigned bad, unsigned code, unsigned lr)
{
    PrintConsole *pc;
    char t[48];
    unsigned a = bad & ~3u;
    const unsigned char *tb = reg_t;
    HBlk *own;
    int f, q = 0;
    heap_halt = 1;
    probe_dead = 1;
    own = reg_owner(a, hb_live, &f);
    if (!f) {
        own = reg_owner(a, hb_qhead, &f);
        q = f;
    }
    pc = consoleDemoInit();
    BG_PALETTE_SUB[0] = RGB15(0, 10, 0);
    exc_puts(pc, 0, 0, "LUA REGISTRO DANADO");
    snprintf(t, sizeof t, "en %08X", bad);
    exc_puts(pc, 0, 2, t);
    if (hb_ptr_ok((const void *)a)) {
        const unsigned *w = (const unsigned *)a;
        snprintf(t, sizeof t, "%08X %08X %08X", w[0], w[1], w[2]);
        exc_puts(pc, 0, 3, t);
    }
    snprintf(t, sizeof t, "tabla %08X lim %u lsn %u", (unsigned)tb, *(const unsigned *)(tb + 8), (unsigned)tb[7]);
    exc_puts(pc, 0, 5, t);
    snprintf(t, sizeof t, "arr %08X node %08X", *(const unsigned *)(tb + 12), *(const unsigned *)(tb + 16));
    exc_puts(pc, 0, 6, t);
    snprintf(t, sizeof t, "bien c%u lr%08X", probe_ok_code, probe_ok_lr);
    exc_puts(pc, 0, 8, t);
    snprintf(t, sizeof t, "mal  c%u lr%08X", code, lr);
    exc_puts(pc, 0, 9, t);
    if (own) {
        snprintf(t, sizeof t, "blq %08X %s t%u", (unsigned)(own + 1), q ? "LIBRE" : "VIVO", own->size);
        exc_puts(pc, 0, 11, t);
        snprintf(t, sizeof t, "a %08X %08X %08X", own->alloc_lr, own->a2, own->a3);
        exc_puts(pc, 0, 12, t);
        snprintf(t, sizeof t, "f %08X %08X %08X", own->free_lr, own->f2, own->f3);
        exc_puts(pc, 0, 13, t);
        snprintf(t, sizeof t, "seq %u de %u", own->seq, hb_seq);
        exc_puts(pc, 0, 14, t);
    } else {
        exc_puts(pc, 0, 11, "blq: ninguno");
    }
    snprintf(t, sizeof t, "comprob %u fread %u", reg_checks, rd_cnt);
    exc_puts(pc, 0, 15, t);
    snprintf(t, sizeof t, "G %08X go%u ro%u", reg_G, reg_goff, reg_roff);
    exc_puts(pc, 0, 16, t);
    exc_puts(pc, 0, 17, "ok:");
    exc_puts(pc, 3, 17, probe_prev);
    snprintf(t, sizeof t, "vid %08X-%08X", (unsigned)video_pixels, (unsigned)video_pixels + SCREEN_BYTES);
    exc_puts(pc, 0, 19, t);
    snprintf(t, sizeof t, "W%d H%d V%d L-vid %d", &nds_gnW ? (int)nds_gnW : -1, &nds_gnH ? (int)nds_gnH : -1,
        &nds_gnV ? (int)nds_gnV : -1, (int)((unsigned)reg_L - (unsigned)video_pixels));
    exc_puts(pc, 0, 20, t);
    for (;;) {
    }
}

size_t __wrap_fread(void *ptr, size_t size, size_t n, FILE *f)
{
    unsigned lr = (unsigned)__builtin_return_address(0);
    size_t r;
    rd_cnt++;
    nds_lua_light(9, lr);
    nds_audio_update();
    r = __real_fread(ptr, size, n, f);
    nds_lua_light(10, lr);
    return r;
}


/* ---------- muros alrededor del bufer de pantalla ---------- */
#define VPRE 16384
#define VPOST 65536
static unsigned char *video_raw;
static volatile unsigned vw_ok_code, vw_ok_lr;

static int vw_scan(const unsigned char *b, int n, int base, int *lo, int *hi)
{
    int i, cnt = 0;
    for (i = 0; i < n; i += 4) {
        if (*(const unsigned *)(b + i) != 0xA5A5A5A5u) {
            int k;
            for (k = 0; k < 4; k++) {
                if (b[i + k] != 0xA5) {
                    if (!cnt)
                        *lo = base + i + k;
                    *hi = base + i + k;
                    cnt++;
                }
            }
        }
    }
    return cnt;
}

static void vwall_check(unsigned code, unsigned lr)
{
    if (!NDS_DEBUG)
        return;
    int lo1 = 0, hi1 = 0, lo2 = 0, hi2 = 0, n1, n2, lo, row, col;
    PrintConsole *pc;
    char t[48];
    if (!video_raw || heap_halt)
        return;
    n1 = vw_scan(video_raw, VPRE, -VPRE, &lo1, &hi1);
    n2 = vw_scan(video_raw + VPRE + SCREEN_BYTES, VPOST, SCREEN_BYTES, &lo2, &hi2);
    guard_check();
    if (guard_bad && !n1 && !n2) {
        heap_halt = 1;
        probe_dead = 1;
        pc = consoleDemoInit();
        BG_PALETTE_SUB[0] = RGB15(12, 6, 0);
        exc_puts(pc, 0, 0, "MURO SUPERFICIE PISADO");
        snprintf(t, sizeof t, "bien c%u lr%08X", vw_ok_code, vw_ok_lr);
        exc_puts(pc, 0, 2, t);
        snprintf(t, sizeof t, "mal  c%u lr%08X", code, lr);
        exc_puts(pc, 0, 3, t);
        snprintf(t, sizeof t, "superficie %dx%d", guard_w, guard_h);
        exc_puts(pc, 0, 5, t);
        snprintf(t, sizeof t, "antes %u bytes  despues %u", guard_pre, guard_post);
        exc_puts(pc, 0, 6, t);
        exc_puts(pc, 0, 8, "ok:");
        exc_puts(pc, 3, 8, probe_prev);
        for (;;) {
        }
    }
    if (!n1 && !n2) {
        vw_ok_code = code;
        vw_ok_lr = lr;
        return;
    }
    heap_halt = 1;
    probe_dead = 1;
    pc = consoleDemoInit();
    BG_PALETTE_SUB[0] = RGB15(12, 6, 0);
    exc_puts(pc, 0, 0, "MURO VIDEO PISADO");
    snprintf(t, sizeof t, "bien c%u lr%08X", vw_ok_code, vw_ok_lr);
    exc_puts(pc, 0, 2, t);
    snprintf(t, sizeof t, "mal  c%u lr%08X", code, lr);
    exc_puts(pc, 0, 3, t);
    snprintf(t, sizeof t, "antes: n%d %d..%d", n1, lo1, hi1);
    exc_puts(pc, 0, 5, t);
    snprintf(t, sizeof t, "despues: n%d %d..%d", n2, lo2, hi2);
    exc_puts(pc, 0, 6, t);
    lo = n1 ? lo1 : lo2;
    row = lo >= 0 ? lo / SCREEN_W : -((-lo + SCREEN_W - 1) / SCREEN_W);
    col = lo - row * SCREEN_W;
    snprintf(t, sizeof t, "primero fila %d col %d", row, col);
    exc_puts(pc, 0, 8, t);
    if (n2) {
        int hrow = hi2 / SCREEN_W;
        snprintf(t, sizeof t, "ultimo  fila %d col %d", hrow, hi2 - hrow * SCREEN_W);
        exc_puts(pc, 0, 9, t);
    }
    {
        const unsigned char *b = video_pixels + lo;
        snprintf(t, sizeof t, "bytes %02X %02X %02X %02X %02X %02X", b[0], b[1], b[2], b[3], b[4], b[5]);
        exc_puts(pc, 0, 11, t);
    }
    snprintf(t, sizeof t, "W%d H%d V%d", &nds_gnW ? (int)nds_gnW : -1, &nds_gnH ? (int)nds_gnH : -1,
        &nds_gnV ? (int)nds_gnV : -1);
    exc_puts(pc, 0, 13, t);
    exc_puts(pc, 0, 15, "ok:");
    exc_puts(pc, 3, 15, probe_prev);
    for (;;) {
    }
}

void nds_lua_check(unsigned code)
{
    vwall_check(code, (unsigned)__builtin_return_address(0));
    nds_lua_light(code, (unsigned)__builtin_return_address(0));
}

Uint32 SDL_GetTicks(void)
{
    nds_lua_light(3, (unsigned)__builtin_return_address(0));
    u16 now;
    static unsigned yield_cnt;
    hb_ticks++;
    if ((++yield_cnt & 31) == 0) {
        nds_audio_update();
        thrd_yield();
    }
    if (!timer_ready)
        timer_init();
    now = TIMER_DATA(1);
    if (now < tick_last)
        tick_hi += 0x10000;
    tick_last = now;
    return tick_hi + now;
}

/* NDS-MP4: la red local (protocol_nds.cpp) se atiende desde aqui y desde SDL_Flip */
void (*nds_net_hook)(void) = 0;

void SDL_Delay(Uint32 ms)
{
    nds_lua_light(6, (unsigned)__builtin_return_address(0));
    hb_delay++;
    hb_last = "Delay";
    Uint32 end = SDL_GetTicks() + ms;
    if (nds_net_hook) /* NDS-MP4 */
        nds_net_hook();
    do {
        thrd_yield();
    } while ((Sint32)(end - SDL_GetTicks()) > 0);
}

/* ---------------- superficies 8 bpp ---------------- */
typedef struct {
    SDL_Surface s; /* debe ser el primer miembro */
    SDL_PixelFormat f;
    SDL_Palette p;
    SDL_Color c[256];
    void *own_pixels;
} NdsSurface;

static SDL_Surface video_surface;
static SDL_PixelFormat video_format;
static SDL_Palette video_palette;
static SDL_Color video_colors[256];
static Uint8 *video_pixels;
static void *video_vram;
static void *video_vram_sub;
static int video_ready;

#define GUARD_PAD 16384
#define GUARD_MIN 40000
#define GUARD_MAX 8
static struct { unsigned char *raw; size_t sz; int w, h; } guard_tab[GUARD_MAX];

static void *guard_alloc(size_t sz, int w, int h, void **rawout)
{
    int i;
    unsigned char *raw;
    if (NDS_DEBUG && sz >= GUARD_MIN) { /* NDS-MEMNOMBRES: sin guardas en la version normal */
        for (i = 0; i < GUARD_MAX; i++) {
            if (guard_tab[i].raw)
                continue;
            raw = (unsigned char *)calloc(1, sz + 2 * GUARD_PAD);
            if (!raw)
                return NULL;
            memset(raw, 0xA5, GUARD_PAD);
            memset(raw + GUARD_PAD + sz, 0xA5, GUARD_PAD);
            guard_tab[i].raw = raw;
            guard_tab[i].sz = sz;
            guard_tab[i].w = w;
            guard_tab[i].h = h;
            *rawout = raw;
            return raw + GUARD_PAD;
        }
    }
    raw = (unsigned char *)calloc(1, sz);
    *rawout = raw;
    return raw;
}

static void guard_scan(int i)
{
    const unsigned *pre = (const unsigned *)guard_tab[i].raw;
    const unsigned *post = (const unsigned *)(guard_tab[i].raw + GUARD_PAD + guard_tab[i].sz);
    int k, lo = -1, hi = -1;
    for (k = 0; k < GUARD_PAD / 4; k++)
        if (pre[k] != 0xA5A5A5A5u && lo < 0)
            lo = k;
    for (k = 0; k < GUARD_PAD / 4; k++)
        if (post[k] != 0xA5A5A5A5u)
            hi = k;
    if ((lo >= 0 || hi >= 0) && !guard_bad) {
        guard_w = guard_tab[i].w;
        guard_h = guard_tab[i].h;
        guard_pre = lo >= 0 ? (unsigned)(GUARD_PAD - lo * 4) : 0u;
        guard_post = hi >= 0 ? (unsigned)((hi + 1) * 4) : 0u;
        guard_bad = 1;
    }
}

static void guard_check(void)
{
    if (!NDS_DEBUG)
        return;
    int i;
    for (i = 0; i < GUARD_MAX; i++)
        if (guard_tab[i].raw)
            guard_scan(i);
}

static void guard_release(void *raw)
{
    int i;
    for (i = 0; i < GUARD_MAX; i++) {
        if (guard_tab[i].raw && (void *)guard_tab[i].raw == raw) {
            guard_scan(i);
            guard_tab[i].raw = NULL;
        }
    }
}

void nds_mem_name(const void *p, const char *name); /* NDS-MEMNOMBRES */
static SDL_Surface *new_surface8(int w, int h, int pitch, void *pixels)
{
    NdsSurface *n = (NdsSurface *)calloc(1, sizeof(NdsSurface));
    if (!n) {
        SDL_SetError("sin memoria");
        return NULL;
    }
    if (!pixels) {
        void *raw = NULL;
        pitch = (w + 3) & ~3;
        pixels = guard_alloc((size_t)pitch * (size_t)(h > 0 ? h : 1), w, h, &raw);
        if (!pixels) {
            free(n);
            SDL_SetError("sin memoria");
            return NULL;
        }
        n->own_pixels = raw;
        { /* NDS-MEMNOMBRES */
            char nm[32];
            snprintf(nm, sizeof nm, "superficie %dx%d", w, h);
            nds_mem_name(raw, nm);
        }
    }
    n->p.ncolors = 256;
    n->p.colors = n->c;
    n->f.palette = &n->p;
    n->f.BitsPerPixel = 8;
    n->f.BytesPerPixel = 1;
    n->s.flags = SDL_SWSURFACE;
    n->s.format = &n->f;
    n->s.w = w;
    n->s.h = h;
    n->s.pitch = (Uint16)pitch;
    n->s.pixels = pixels;
    n->s.clip_rect.x = 0;
    n->s.clip_rect.y = 0;
    n->s.clip_rect.w = (Uint16)w;
    n->s.clip_rect.h = (Uint16)h;
    n->s.refcount = 1;
    return &n->s;
}

SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int w, int h, int depth,
    Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    (void)flags; (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    if (depth != 8) {
        SDL_SetError("capa DSi: solo 8 bpp (pedido %d)", depth);
        return NULL;
    }
    if (w <= 0 || h <= 0) {
        SDL_SetError("tamano de superficie invalido");
        return NULL;
    }
    return new_surface8(w, h, 0, NULL);
}

SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int w, int h, int depth, int pitch,
    Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    (void)Rmask; (void)Gmask; (void)Bmask; (void)Amask;
    if (depth != 8 || !pixels) {
        SDL_SetError("capa DSi: solo 8 bpp con memoria propia (pedido %d)", depth);
        return NULL;
    }
    return new_surface8(w, h, pitch, pixels);
}

#define QUAR_N 16
static NdsSurface *quar_n[QUAR_N];
static int quar_head;

void SDL_FreeSurface(SDL_Surface *s)
{
    NdsSurface *n, *old;
    int i;
    if (!s || s == &video_surface)
        return;
    if (--s->refcount > 0)
        return;
    n = (NdsSurface *)s;
    for (i = 0; i < QUAR_N; i++) {
        if (quar_n[i] == n) {
            if (!dbl_free) {
                dbl_w = n->s.w;
                dbl_h = n->s.h;
                dbl_free = 1;
            }
            return;
        }
    }
    old = quar_n[quar_head];
    if (old) {
        guard_release(old->own_pixels);
        free(old->own_pixels);
        free(old);
    }
    quar_n[quar_head] = n;
    quar_head = (quar_head + 1) % QUAR_N;
}

SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, SDL_PixelFormat *fmt, Uint32 flags)
{
    SDL_Surface *d;
    int y;
    (void)flags;
    if (!src || !fmt || src->format->BitsPerPixel != 8 || fmt->BitsPerPixel != 8) {
        SDL_SetError("capa DSi: ConvertSurface solo de 8 a 8 bpp");
        return NULL;
    }
    d = new_surface8(src->w, src->h, 0, NULL);
    if (!d)
        return NULL;
    for (y = 0; y < src->h; y++)
        memcpy((Uint8 *)d->pixels + y * d->pitch, (Uint8 *)src->pixels + y * src->pitch, (size_t)src->w);
    memcpy(d->format->palette->colors, src->format->palette->colors, 256 * sizeof(SDL_Color));
    d->flags |= src->flags & SDL_SRCCOLORKEY;
    d->format->colorkey = src->format->colorkey;
    return d;
}

int SDL_SetColorKey(SDL_Surface *s, Uint32 flag, Uint32 key)
{
    if (!s)
        return -1;
    if (flag & SDL_SRCCOLORKEY) {
        s->flags |= SDL_SRCCOLORKEY;
        s->format->colorkey = key;
    } else {
        s->flags &= ~SDL_SRCCOLORKEY;
    }
    return 0;
}

SDL_bool SDL_SetClipRect(SDL_Surface *s, const SDL_Rect *r)
{
    int x0, y0, x1, y1;
    if (!s)
        return SDL_FALSE;
    if (!r) {
        s->clip_rect.x = 0;
        s->clip_rect.y = 0;
        s->clip_rect.w = (Uint16)s->w;
        s->clip_rect.h = (Uint16)s->h;
        return SDL_TRUE;
    }
    x0 = r->x; y0 = r->y; x1 = r->x + r->w; y1 = r->y + r->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    s->clip_rect.x = (Sint16)x0;
    s->clip_rect.y = (Sint16)y0;
    s->clip_rect.w = (Uint16)(x1 - x0);
    s->clip_rect.h = (Uint16)(y1 - y0);
    return SDL_TRUE;
}

int SDL_FillRect(SDL_Surface *dst, SDL_Rect *r, Uint32 color)
{
    nds_lua_light(2, (unsigned)__builtin_return_address(0));
    int x, y, w, h, c0x, c0y, c1x, c1y;
    if (!dst || dst->format->BitsPerPixel != 8) {
        SDL_SetError("capa DSi: FillRect solo en 8 bpp");
        return -1;
    }
    if (r) { x = r->x; y = r->y; w = r->w; h = r->h; }
    else { x = 0; y = 0; w = dst->w; h = dst->h; }
    c0x = dst->clip_rect.x; c0y = dst->clip_rect.y;
    c1x = c0x + dst->clip_rect.w; c1y = c0y + dst->clip_rect.h;
    if (x < c0x) { w -= c0x - x; x = c0x; }
    if (y < c0y) { h -= c0y - y; y = c0y; }
    if (x + w > c1x) w = c1x - x;
    if (y + h > c1y) h = c1y - y;
    if (w <= 0 || h <= 0) {
        if (r) { r->w = 0; r->h = 0; }
        return 0;
    }
    for (int j = 0; j < h; j++)
        memset((Uint8 *)dst->pixels + (y + j) * dst->pitch + x, (int)(color & 0xFF), (size_t)w);
    if (r) { r->x = (Sint16)x; r->y = (Sint16)y; r->w = (Uint16)w; r->h = (Uint16)h; }
    return 0;
}

int SDL_UpperBlit(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    nds_lua_light(1, (unsigned)__builtin_return_address(0));
    int sx, sy, w, h, dx, dy, i, j;
    const SDL_Rect *c;
    if (!src || !dst) {
        SDL_SetError("Blit con superficie NULL");
        return -1;
    }
    if (src->format->BitsPerPixel != 8 || dst->format->BitsPerPixel != 8) {
        SDL_SetError("capa DSi: Blit solo entre superficies de 8 bpp");
        return -1;
    }
    if (srcrect) { sx = srcrect->x; sy = srcrect->y; w = srcrect->w; h = srcrect->h; }
    else { sx = 0; sy = 0; w = src->w; h = src->h; }
    dx = dstrect ? dstrect->x : 0;
    dy = dstrect ? dstrect->y : 0;
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    c = &dst->clip_rect;
    if (dx < c->x) { int d = c->x - dx; sx += d; w -= d; dx = c->x; }
    if (dy < c->y) { int d = c->y - dy; sy += d; h -= d; dy = c->y; }
    if (dx + w > c->x + c->w) w = c->x + c->w - dx;
    if (dy + h > c->y + c->h) h = c->y + c->h - dy;
    if (w <= 0 || h <= 0) {
        if (dstrect) { dstrect->w = 0; dstrect->h = 0; }
        return 0;
    }
    if (src->flags & SDL_SRCCOLORKEY) {
        Uint8 key = (Uint8)src->format->colorkey;
        for (j = 0; j < h; j++) {
            const Uint8 *s = (const Uint8 *)src->pixels + (sy + j) * src->pitch + sx;
            Uint8 *d = (Uint8 *)dst->pixels + (dy + j) * dst->pitch + dx;
            for (i = 0; i < w; i++)
                if (s[i] != key)
                    d[i] = s[i];
        }
    } else {
        for (j = 0; j < h; j++)
            memmove((Uint8 *)dst->pixels + (dy + j) * dst->pitch + dx,
                (const Uint8 *)src->pixels + (sy + j) * src->pitch + sx, (size_t)w);
    }
    if (dstrect) { dstrect->x = (Sint16)dx; dstrect->y = (Sint16)dy; dstrect->w = (Uint16)w; dstrect->h = (Uint16)h; }
    return 0;
}

int SDL_SoftStretch(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
    (void)src; (void)srcrect; (void)dst; (void)dstrect;
    SDL_SetError("capa DSi: SoftStretch no implementado");
    return -1;
}

/* ---------------- video ---------------- */
static int nds_put_null(char c, FILE *f) /* NDS-RAYA: salida de texto descartada */
{
    (void)f;
    return (unsigned char)c;
}

SDL_Surface *SDL_SetVideoMode(int w, int h, int bpp, Uint32 flags)
{
    (void)w; (void)h; (void)bpp; (void)flags; /* siempre se entrega 256x192x8 */
    if (!video_ready) {
        int bg;
        video_raw = (unsigned char *)memalign(32, VPRE + SCREEN_BYTES + VPOST);
        if (video_raw) {
            memset(video_raw, 0xA5, VPRE + SCREEN_BYTES + VPOST);
            video_pixels = (Uint8 *)(video_raw + VPRE);
        } else {
            video_pixels = NULL;
        }
        if (!video_pixels) {
            SDL_SetError("sin memoria para la pantalla");
            return NULL;
        }
        memset(video_pixels, 0, SCREEN_BYTES);
        nds_apply_swap(); /* NDS-OPCIONES: el juego va en la tactil salvo que se intercambien las pantallas */
        videoSetMode(MODE_5_2D | DISPLAY_BG3_ACTIVE);
        vramSetBankA(VRAM_A_MAIN_BG);
        bg = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
        video_vram = (void *)bgGetGfxPtr(bg);
        if (!NDS_DEBUG) {
            int bgs;
            videoSetModeSub(MODE_5_2D | DISPLAY_BG3_ACTIVE);
            vramSetBankC(VRAM_C_SUB_BG);
            bgs = bgInitSub(3, BgType_Bmp8, BgSize_B8_256x256, 4, 0);
            video_vram_sub = (void *)bgGetGfxPtr(bgs);
            stdout->put = nds_put_null; /* NDS-RAYA: la consola ya no se ve; que no pinte en la VRAM de arriba */
            if (stderr != stdout)
                stderr->put = nds_put_null;
            dmaFillWords(0, video_vram_sub, SCREEN_W * LCD_H);
        }
        video_palette.ncolors = 256;
        video_palette.colors = video_colors;
        video_format.palette = &video_palette;
        video_format.BitsPerPixel = 8;
        video_format.BytesPerPixel = 1;
        video_surface.flags = SDL_SWSURFACE | SDL_HWPALETTE | SDL_FULLSCREEN;
        video_surface.format = &video_format;
        video_surface.w = SCREEN_W;
        video_surface.h = SCREEN_H;
        video_surface.pitch = SCREEN_W;
        video_surface.pixels = video_pixels;
        video_surface.clip_rect.x = 0;
        video_surface.clip_rect.y = 0;
        video_surface.clip_rect.w = SCREEN_W;
        video_surface.clip_rect.h = SCREEN_H;
        video_surface.refcount = 1;
        video_ready = 1;
    }
    return &video_surface;
}

SDL_Surface *SDL_GetVideoSurface(void) { return video_ready ? &video_surface : NULL; }

static SDL_Rect mode_256x192 = { 0, 0, SCREEN_W, SCREEN_H };
static SDL_Rect *mode_list[2] = { &mode_256x192, NULL };
SDL_Rect **SDL_ListModes(SDL_PixelFormat *format, Uint32 flags)
{
    (void)format; (void)flags;
    return mode_list;
}
int SDL_VideoModeOK(int w, int h, int bpp, Uint32 flags)
{
    (void)w; (void)h; (void)bpp; (void)flags;
    return 8;
}

int SDL_SetPalette(SDL_Surface *s, int flags, SDL_Color *colors, int first, int n)
{
    nds_lua_light(5, (unsigned)__builtin_return_address(0));
    int i;
    (void)flags;
    if (!s || !s->format || !s->format->palette || !colors)
        return 0;
    if (first < 0 || n < 0 || first + n > s->format->palette->ncolors)
        return 0;
    memcpy(&s->format->palette->colors[first], colors, (size_t)n * sizeof(SDL_Color));
    if (s == &video_surface)
        for (i = 0; i < n; i++)
        {
            u16 c15 = RGB15(colors[i].r >> 3, colors[i].g >> 3, colors[i].b >> 3);
            BG_PALETTE[first + i] = c15;
            if (video_vram_sub)
                BG_PALETTE_SUB[first + i] = c15;
            pal_lum[first + i] = (Uint8)((colors[i].r * 2 + colors[i].g * 5 + colors[i].b) >> 3);
        }
    return 1;
}

/* NDS-RAPIDO1: composicion en modo ARM con -O2 (hasta el medidor) */
#pragma GCC push_options
#pragma GCC optimize("O2")
#pragma GCC target("arm")
/* ---------- paneles del juego reducidos en la pantalla tactil ---------- */
#define UI_X0 448
#define UI_PW 174 /* ancho del panel 320 reducido 6/11 */
static const unsigned char *ui_pix;
static int ui_pitch, ui_mode, ui_w, ui_h;
/* NDS-SHARP: al reducir se elige el pixel mas brillante de cada bloque, asi no se pierden los trazos del texto */
#define NDS_UI_SHARP 1

static inline Uint8 sharp_pick(const unsigned char *s, int pitch, int bw, int bh)
{
    Uint8 best = s[0];
    Uint8 bl = pal_lum[best];
    int i, j;
    for (j = 0; j < bh; j++) {
        const unsigned char *r = s + j * pitch;
        for (i = 0; i < bw; i++) {
            Uint8 c = r[i];
            if (pal_lum[c] > bl) {
                bl = pal_lum[c];
                best = c;
            }
        }
    }
    return best;
}
static Uint8 comp_buf[LCD_W * LCD_H] __attribute__((aligned(32)));
static Uint8 *vback; /* NDS-RAPIDO4: pagina de VRAM donde se escribe el mundo (modo 0) */

#define CH_SPLIT 105 /* NDS-PERSONAJE4 (antes 111/118): filas 0-117 del panel del personaje arriba, 118-337 en la tactil */
static int ui_wide; /* NDS-PANELANCHO: 1 = personaje (izquierda), 2 = libro de hechizos (derecha) mas anchos */
void nds_ui_wide(int w) { ui_wide = w; }
static int ui_gmenu; /* NDS-MENUTRANS: menu del juego abierto: el color 0 es transparente */
void nds_ui_gmenu(int on) { ui_gmenu = on; }
static Uint8 dark_lut[256]; /* NDS-TIENDA6: cada color de la paleta oscurecido al 35% */
static void dark_build(void)
{
    int i, j;
    for (i = 0; i < 256; i++) {
        const u16 c = BG_PALETTE[i];
        const int r = (c & 31) * 35 / 100, g = ((c >> 5) & 31) * 35 / 100, b = ((c >> 10) & 31) * 35 / 100;
        int best = 0, bd = 1 << 30;
        for (j = 0; j < 256; j++) {
            const u16 cc = BG_PALETTE[j];
            const int dr = (cc & 31) - r, dg = ((cc >> 5) & 31) - g, db = ((cc >> 10) & 31) - b;
            const int dd = dr * dr + dg * dg + db * db;
            if (dd < bd) { bd = dd; best = j; }
        }
        dark_lut[i] = (Uint8)best;
    }
}
static int ui_dialog; /* NDS-DIALOGO: hay un dialogo (qtext): la interfaz se baja DLG_OFF filas */
#define DLG_OFF 30
void nds_ui_dialog(int on) { ui_dialog = on; }
static int ui_spells; /* NDS-HECHIZOS: lista de hechizos rapidos abierta */
void nds_ui_spells(int on) { ui_spells = on; }
static int ui_store; /* NDS-TIENDA2: la tienda esta abierta (recorte ampliado del recuadro) */
void nds_ui_store(int on) { ui_store = on; }
static int st_live; /* NDS-TIENDATXT: >0 = la tienda se dibuja con la letra 8x8 */
static void st_compose(void);
static int st_touch(u16 *px, u16 *py);
#define ST_SX 336 /* recuadro de la tienda en la mitad derecha de la interfaz (NDS-TIENDA2b) */
#define ST_SY 20
#define ST_SH 318 /* filas de origen (20-337) que ocupan las 192 de la tactil */
#define ST_SW 290 /* NDS-TIENDA3: columnas de origen (336-625) */
#define ST_DW 256 /* NDS-TIENDA5: recuadro de tienda/NPC a todo el ancho (antes 215) */
void nds_ui_frame(const unsigned char *pixels, int pitch, int mode)
{
    ui_pix = pixels;
    ui_pitch = pitch;
    ui_mode = mode;
}

void nds_ui_frame_scaled(const unsigned char *pixels, int pitch, int w, int h)
{
    ui_pix = pixels;
    ui_pitch = pitch;
    ui_w = w;
    ui_h = h;
    ui_mode = pixels ? 5 : 0;
}

/* NDS-CINTURON: cinturon (8 casillas de 29x29 del panel, x 205+29*i, y 5) a la izquierda de la tactil, 2x4 */
#define BL_X 12
#define BL_Y 44
#define BL_S 29
static void belt_draw(void);
static int belt_hit(int x, int y, int *slot, int *lx, int *ly)
{
    int c, r;
    if (x < BL_X || x >= BL_X + 2 * BL_S || y < BL_Y || y >= BL_Y + 4 * BL_S)
        return 0;
    c = (x - BL_X) / BL_S;
    r = (y - BL_Y) / BL_S;
    if (slot) *slot = r * 2 + c;
    if (lx) { *lx = (x - BL_X) % BL_S; if (*lx < 1) *lx = 1; if (*lx > 27) *lx = 27; }
    if (ly) { *ly = (y - BL_Y) % BL_S; if (*ly < 1) *ly = 1; if (*ly > 27) *ly = 27; }
    return 1;
}
static int belt_on(void) { return ui_mode == 2 && !ui_store && !(ui_wide & 2); }

/* NDS-DIALOGO2: dialogo a pantalla completa: zona 452x339 de la interfaz -> 256x192, fondo = mundo oscurecido */
#define DQ_SX 94
#define DQ_SW 452
#define DQ_SH 339
static int vk_near(int r, int g, int b);
static void dlg_compose(void)
{
    static int ok, col;
    static unsigned tick;
    int x, y;
    if (!ok || ++tick >= 120) {
        dark_build();
        col = vk_near(24, 18, 6);
        tick = 0;
        ok = 1;
    }
    for (y = 0; y < LCD_H; y++) {
        const int sy = y * DQ_SH / LCD_H;
        const unsigned char *s0 = ui_pix + sy * ui_pitch + DQ_SX;
        const unsigned char *s1 = s0 + (sy + 1 < DQ_SH ? ui_pitch : 0);
        Uint8 *d = comp_buf + y * LCD_W;
        for (x = 0; x < LCD_W; x++) {
            const int sx = x * DQ_SW / LCD_W;
            Uint8 a = s0[sx], b = s0[sx + 1], c = s1[sx], e = s1[sx + 1];
            if (pal_lum[b] > pal_lum[a]) a = b;
            if (pal_lum[c] > pal_lum[a]) a = c;
            if (pal_lum[e] > pal_lum[a]) a = e;
            d[x] = a ? a : dark_lut[d[x]];
        }
    }
    for (x = 1; x < LCD_W - 1; x++) { /* marco dorado */
        comp_buf[1 * LCD_W + x] = (Uint8)col;
        comp_buf[(LCD_H - 2) * LCD_W + x] = (Uint8)col;
    }
    for (y = 1; y < LCD_H - 1; y++) {
        comp_buf[y * LCD_W + 1] = (Uint8)col;
        comp_buf[y * LCD_W + LCD_W - 2] = (Uint8)col;
    }
}

/* NDS-HECHIZOS: iconos (x 12..459, filas 0..335 de la interfaz) a toda la tactil, escala 1,75; fondo = mundo */
#define SP_SX 12
#define SP_BOT 336
static void spl_compose(void)
{
    int x, y;
    for (y = 0; y < LCD_H; y++) {
        const int sy = SP_BOT - (LCD_H - y) * 7 / 4;
        const unsigned char *s;
        Uint8 *d = comp_buf + y * LCD_W;
        if (sy < 0)
            continue;
        s = ui_pix + sy * ui_pitch + SP_SX;
        for (x = 0; x < LCD_W; x++) {
            const Uint8 v = s[x * 7 / 4];
            if (v)
                d[x] = v;
        }
    }
}

/* NDS-MISIONES: zona interior del registro de misiones a toda la tactil */
#define QL_X0 0 /* NDS-MISIONES2: todo el ancho */
#define QL_W 318
#define QL_Y0 20
#define QL_H 312
static int nds_lang_es; /* NDS-IDIOMA: 1 = textos propios en espanol */
void nds_set_lang_es(int on) { nds_lang_es = on; }
static void nds_ui_compose(void)
{
    int x, y;
    if (ui_mode == 5 && ui_w == LCD_W && ui_h == LCD_H) { /* NDS-DSV: cuadro ya a 256x192: copia directa */
        for (y = 0; y < LCD_H; y++)
            memcpy(comp_buf + y * LCD_W, ui_pix + y * ui_pitch, LCD_W);
        return;
    }
    if (ui_mode == 5) { /* video: escalar el cuadro entero a 256x192 */
        for (y = 0; y < LCD_H; y++) {
            const unsigned char *s = ui_pix + (y * ui_h / LCD_H) * ui_pitch;
            Uint8 *d = comp_buf + y * LCD_W;
            for (x = 0; x < LCD_W; x++)
                d[x] = s[x * ui_w / LCD_W];
        }
        return;
    }
    { /* NDS-ZOOM: mundo WORLD_W x WORLD_H reducido a 256x192 */
        static u16 zx[LCD_W], zy[LCD_H];
        static int zinit;
        if (!zinit) {
            for (x = 0; x < LCD_W; x++)
                zx[x] = (u16)(x * WORLD_W / LCD_W);
            for (y = 0; y < LCD_H; y++)
                zy[y] = (u16)(y * WORLD_H / LCD_H);
            zinit = 1;
        }
        for (y = 0; y < LCD_H; y++) { /* NDS-RAPIDO2: 4 pixeles por escritura */
            const Uint8 *sr = video_pixels + zy[y] * SCREEN_W;
            u32 *d = (u32 *)((ui_mode == 0 && vback ? vback : comp_buf) + y * LCD_W); /* NDS-RAPIDO4 */
            for (x = 0; x < LCD_W; x += 4)
                *d++ = (u32)sr[zx[x]] | ((u32)sr[zx[x + 1]] << 8) | ((u32)sr[zx[x + 2]] << 16) | ((u32)sr[zx[x + 3]] << 24);
        }
    }
    if (ui_mode == 3 && ui_dialog) { /* NDS-DIALOGO2 */
        dlg_compose();
        return;
    }
    if (ui_mode == 3 && ui_spells) { /* NDS-HECHIZOS */
        spl_compose();
        return;
    }
    if (st_live > 0) { /* NDS-TIENDATXT */
        st_compose();
        return;
    }
    if (ui_mode >= 3) {
        for (y = 0; y < LCD_H; y++) {
            const int sy = (ui_mode == 3 && ui_dialog) ? y - DLG_OFF : y; /* NDS-DIALOGO: cuadro centrado */
            const unsigned char *s = ui_pix + (sy * 5 / 2) * ui_pitch;
            Uint8 *d = comp_buf + y * LCD_W;
            if (sy < 0)
                continue;
            if (ui_mode == 3 && sy * 5 / 2 + 2 >= 339) /* NDS-HUD4: ni panel ni parte alta de los frascos */
                continue;
            for (x = 0; x < LCD_W; x++)
            { /* NDS-MENUTRANS: con el menu del juego, sin fondo negro */
                const Uint8 v = NDS_UI_SHARP ? sharp_pick(s + x * 5 / 2, ui_pitch, 2, 2) : s[x * 5 / 2];
                if (v || !ui_gmenu)
                    d[x] = v;
            }
        }
    } else if (ui_mode == 1 && (ui_wide & 4)) { /* NDS-MISIONES */
        for (y = 0; y < LCD_H; y++) {
            const unsigned char *s0 = ui_pix + (QL_Y0 + y * QL_H / LCD_H) * ui_pitch + QL_X0;
            const unsigned char *s1 = s0 + ui_pitch;
            Uint8 *d = comp_buf + y * LCD_W;
            for (x = 0; x < LCD_W; x++) {
                const int sx = x * QL_W / LCD_W;
                Uint8 a = s0[sx], b = s0[sx + 1], c = s1[sx], e = s1[sx + 1];
                if (pal_lum[b] > pal_lum[a]) a = b;
                if (pal_lum[c] > pal_lum[a]) a = c;
                if (pal_lum[e] > pal_lum[a]) a = e;
                d[x] = a;
            }
        }
        { /* NDS-MISIONES2: marco dorado */
            const Uint8 col = (Uint8)vk_near(24, 18, 6);
            for (x = 1; x < LCD_W - 1; x++) {
                comp_buf[1 * LCD_W + x] = col;
                comp_buf[(LCD_H - 2) * LCD_W + x] = col;
            }
            for (y = 1; y < LCD_H - 1; y++) {
                comp_buf[y * LCD_W + 1] = col;
                comp_buf[y * LCD_W + LCD_W - 2] = col;
            }
        }
    } else if (ui_mode == 1 || ui_mode == 2) {
        int dx0 = ui_mode == 1 ? 0 : LCD_W - UI_PW;
        int sx0 = ui_mode == 1 ? 0 : 320;
        if ((ui_mode == 1 && (ui_wide & 1)) || (ui_mode == 2 && (ui_wide & 2))) { /* NDS-PANELANCHO: 215 de ancho, pixel mas brillante de cada 2x2 */
            const int wx0 = 0; /* NDS-LIBRO2: el libro tambien a todo el ancho */
            const int dw = LCD_W; /* NDS-PERSONAJE2 / LIBRO2: a todo el ancho */
            for (y = 0; y < LCD_H; y++) {
                const int sy = ui_mode == 1 ? CH_SPLIT + y * (338 - CH_SPLIT) / LCD_H : 14 + y * 336 / LCD_H; /* NDS-PERSONAJE7a / LIBRO2: libro filas 14-349 (pestanas enteras) */
                const unsigned char *s0, *s1;
                Uint8 *d;
                if (ui_mode == 1 && y >= LCD_H) { /* NDS-PERSONAJE7a: ya no sobran filas */ /* NDS-PERSONAJE4: filas sobrantes en negro */
                    memset(comp_buf + y * LCD_W, 0, LCD_W);
                    continue;
                }
                s0 = ui_pix + sy * ui_pitch + sx0;
                s1 = s0 + ui_pitch;
                d = comp_buf + y * LCD_W + wx0;
                for (x = 0; x < dw; x++) {
                    const int sx = ui_mode == 1 ? x * 320 / dw : 4 + x * 312 / dw; /* NDS-LIBRO2: sin 4 columnas de borde */
                    Uint8 a = s0[sx], b = s0[sx + 1], c = s1[sx], e = s1[sx + 1];
                    if (pal_lum[b] > pal_lum[a]) a = b;
                    if (pal_lum[c] > pal_lum[a]) a = c;
                    if (pal_lum[e] > pal_lum[a]) a = e;
                    d[x] = a;
                }
            }
            return;
        }
        if (ui_mode == 2 && ui_store) { /* NDS-TIENDA2/3: recuadro de la tienda ampliado, el pixel mas brillante de cada 2x2 */
            { static int dark_ok; static unsigned dark_tick; /* NDS-TIENDA6 */
              if (!dark_ok || ++dark_tick >= 120) { dark_build(); dark_tick = 0; dark_ok = 1; } }
            for (y = 0; y < LCD_H; y++) {
                const unsigned char *s0 = ui_pix + (ST_SY + y * ST_SH / LCD_H) * ui_pitch + ST_SX;
                const unsigned char *s1 = s0 + ui_pitch;
                Uint8 *d = comp_buf + y * LCD_W + (LCD_W - ST_DW) / 2; /* NDS-TIENDA4: recuadro centrado */
                for (x = 0; x < ST_DW; x++) {
                    const int sx = x * ST_SW / ST_DW;
                    Uint8 a = s0[sx], b = s0[sx + 1], c = s1[sx], e = s1[sx + 1];
                    if (pal_lum[b] > pal_lum[a]) a = b;
                    if (pal_lum[c] > pal_lum[a]) a = c;
                    if (pal_lum[e] > pal_lum[a]) a = e;
                    d[x] = a ? a : dark_lut[d[x]]; /* NDS-TIENDA6: fondo negro = mundo oscurecido */
                }
            }
            return;
        }
        for (y = 0; y < LCD_H; y++) {
            const unsigned char *s = ui_pix + (y * 337 / LCD_H) * ui_pitch + sx0; /* NDS-CINTURON2: filas 0-338 a toda la altura */
            Uint8 *d = comp_buf + y * LCD_W + dx0;
            for (x = 0; x < UI_PW; x++)
                d[x] = NDS_UI_SHARP ? sharp_pick(s + x * 11 / 6, ui_pitch, 2, 2) : s[x * 11 / 6];
        }
        if (belt_on()) /* NDS-CINTURON */
            belt_draw();
    }
}

static void nds_touch_map(u16 *px, u16 *py)
{
    int x = *px, y = *py;
    if (ui_mode == 3 && ui_dialog) { /* NDS-DIALOGO2: toque dentro de la zona del texto (cierra el dialogo) */
        *px = (u16)(UI_X0 + DQ_SX + x * DQ_SW / LCD_W);
        *py = (u16)(y * DQ_SH / LCD_H);
        return;
    }
    if (st_touch(px, py)) /* NDS-TIENDATXT */
        return;
    if (ui_mode == 3 && ui_spells) { /* NDS-HECHIZOS */
        *px = (u16)(UI_X0 + SP_SX + x * 7 / 4);
        *py = (u16)(SP_BOT - (LCD_H - y) * 7 / 4);
        return;
    }
    if ((ui_mode == 3 && y * 5 / 2 + 2 < 339) || ui_mode == 4) {
        x = UI_X0 + x * 5 / 2;
        y = y * 5 / 2;
    } else if (ui_mode == 1 && (ui_wide & 4)) { /* NDS-MISIONES */
        x = UI_X0 + QL_X0 + x * QL_W / LCD_W;
        y = QL_Y0 + y * QL_H / LCD_H;
    } else if (ui_mode == 1 && (ui_wide & 1)) { /* NDS-PANELANCHO / PERSONAJE2: personaje a toda la tactil */
        x = UI_X0 + x * 320 / LCD_W;
        y = CH_SPLIT + y * (338 - CH_SPLIT) / LCD_H; /* NDS-PERSONAJE7a */
    } else if (ui_mode == 2 && (ui_wide & 2)) { /* NDS-LIBRO2 */
        x = UI_X0 + 320 + 4 + x * 312 / LCD_W;
        y = 14 + y * 336 / LCD_H;
    } else if (ui_mode == 1 && x < UI_PW) {
        x = UI_X0 + x * 11 / 6;
        y = y * 337 / LCD_H; /* NDS-CINTURON2 */
    } else if (ui_mode == 2 && ui_store && x >= (LCD_W - ST_DW) / 2 && x < (LCD_W - ST_DW) / 2 + ST_DW) { /* NDS-TIENDA2/3/4 */
        x = UI_X0 + ST_SX + (x - (LCD_W - ST_DW) / 2) * ST_SW / ST_DW;
        y = ST_SY + y * ST_SH / LCD_H;
    } else if (ui_mode == 2 && x >= LCD_W - UI_PW) {
        x = UI_X0 + 320 + (x - (LCD_W - UI_PW)) * 11 / 6;
        y = y * 337 / LCD_H; /* NDS-CINTURON2 */
    } else if (belt_on() && belt_hit(x, y, 0, 0, 0)) { /* NDS-CINTURON */
        int slot, lx, ly;
        belt_hit(x, y, &slot, &lx, &ly);
        x = UI_X0 + 205 + slot * BL_S + lx;
        y = 352 + 5 + ly;
    } else { /* NDS-ZOOM: toque sobre el mundo */
        x = x * WORLD_W / LCD_W;
        y = y * WORLD_H / LCD_H;
    }
    *px = (u16)x;
    *py = (u16)y;
}

/* ---------- HUD de la pantalla de arriba (NDS-HUD) ---------- */
/* Origen: panel del juego de 640x128 en la superficie de interfaz.
   Destino (256x192): texto de informacion arriba; abajo vida | cinturon | mana; barra de experiencia bajo el cinturon. */
/* NDS-HUD2: recorte del frasco en el panel (relativo a su esquina) y tamano en pantalla */
#define HUD_FL_SX (-8)   /* columnas a la izquierda del frasco */
#define HUD_FL_SW 104    /* ancho recortado */
#define HUD_FL_SH 141    /* 13 filas sobre el panel + 128 del panel */
#define HUD_FLASK_W 52
#define HUD_FLASK_H 70
#define HUD_BELT_X 52
#define HUD_BELT_W 152
#define HUD_BELT_H 20
#define HUD_BELT_Y 162
#define HUD_XP_Y 185
#define HUD_XP_H 4
#define HUD_INFO_Y 4
static const unsigned char *hud_pan;
static int hud_pitch, hud_xp;
static Uint8 top_buf[LCD_W * LCD_H] __attribute__((aligned(32)));
static Uint8 flask_tmp[HUD_FL_SW * HUD_FL_SH];

/* NDS-TILDES: UTF-8 -> ASCII para la letra 8x8 (solo tiene ASCII) */
static void nds_ascii(char *dst, const char *src, size_t n)
{
    const unsigned char *p = (const unsigned char *)src;
    size_t o = 0;
    while (*p && o + 1 < n) {
        unsigned char c = *p;
        if (c < 0x80) { dst[o++] = (char)c; p++; continue; }
        if (c == 0xC3 && p[1]) {
            unsigned char d = p[1], r = 0;
            if (d >= 0xA0) { d -= 0x20; } /* minuscula -> se recuerda abajo */
            if (d <= 0x85) r = 'A';
            else if (d == 0x87) r = 'C';
            else if (d >= 0x88 && d <= 0x8B) r = 'E';
            else if (d >= 0x8C && d <= 0x8F) r = 'I';
            else if (d == 0x91) r = 'N';
            else if (d >= 0x92 && d <= 0x96) r = 'O';
            else if (d >= 0x99 && d <= 0x9C) r = 'U';
            if (r && p[1] >= 0xA0) r = (unsigned char)(r + 32);
            if (r) dst[o++] = (char)r;
            p += 2;
            continue;
        }
        /* otro caracter multibyte (¿ ¡ etc.): se salta entero */
        p++;
        while ((*p & 0xC0) == 0x80) p++;
    }
    dst[o] = 0;
}
/* NDS-ITEMTXT: descripcion del objeto a tamano real (letra 8x8) en la pantalla de arriba */
#define ITX_MAXL 21
static char itx_buf[640];
static int itx_color, itx_on, itx_age, itx_was;
void nds_item_text(const char *s, int color)
{
    if (!s || !*s) { itx_on = 0; return; }
    nds_ascii(itx_buf, s, sizeof itx_buf); /* NDS-TILDES */
    itx_color = color;
    itx_on = 1;
    itx_age = 0;
}
static int itx_nearest(int r, int g, int b) /* 0-31 cada uno */
{
    int best = 1, bd = 1 << 30, i;
    for (i = 1; i < 256; i++) {
        const u16 cc = BG_PALETTE_SUB[i];
        const int dr = (cc & 31) - r, dg = ((cc >> 5) & 31) - g, db = ((cc >> 10) & 31) - b;
        const int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static void itx_char(int px, int py, unsigned char ch, Uint8 col)
{
    const ConsoleFont *f = &consoleGetDefault()->font;
    const int idx = (int)ch - (int)f->asciiOffset;
    int x, y;
    if (idx < 0 || idx >= (int)f->numChars || px < 0 || px > LCD_W - 8 || py < 0 || py > LCD_H - 8)
        return;
    for (y = 0; y < 8; y++) {
        Uint8 *d = top_buf + (py + y) * LCD_W + px;
        for (x = 0; x < 8; x++) {
            int on;
            if (f->bpp == 1)
                on = (((const u8 *)f->gfx)[idx * 8 + y] >> x) & 1;
            else if (f->bpp == 4) {
                const u8 bb = ((const u8 *)f->gfx)[idx * 32 + y * 4 + (x >> 1)];
                on = (x & 1) ? (bb >> 4) : (bb & 15);
            } else
                on = ((const u8 *)f->gfx)[idx * 64 + y * 8 + x];
            if (on)
                d[x] = col;
        }
    }
}
static void itx_text(int y, const char *t, Uint8 col)
{
    const int n = (int)strlen(t);
    int i, x = (LCD_W - n * 8) / 2;
    for (i = 0; i < n; i++, x += 8)
        itx_char(x, y, (unsigned char)t[i], col);
}
static int itx_compose(void)
{
    char lines[ITX_MAXL][33];
    int n = 0, i, y, h;
    const char *p = itx_buf;
    if (itx_on && ++itx_age > 4) /* NDS-ITEMTXT2 */ /* el motor dejo de mandar texto */
        itx_on = 0;
    if (!itx_on) {
        if (itx_was) { memset(top_buf, 0, sizeof top_buf); itx_was = 0; }
        return 0;
    }
    itx_was = 1;
    memset(top_buf, 0, sizeof top_buf);
    while (n < ITX_MAXL) { /* partir en lineas de 32 letras */
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 0) { lines[n][0] = 0; n++; }
        while (len > 0 && n < ITX_MAXL) {
            size_t take = len > 32 ? 32 : len;
            if (len > 32) { size_t k = take; while (k > 0 && p[k] != ' ') k--; if (k > 0) take = k; }
            memcpy(lines[n], p, take);
            lines[n][take] = 0;
            n++;
            p += take; len -= take;
            while (len > 0 && *p == ' ') { p++; len--; }
        }
        if (!e) break;
        p = e + 1;
    }
    {
        const Uint8 white = (Uint8)itx_nearest(31, 31, 31);
        static const unsigned char cols[4][3] = { { 31, 31, 31 }, { 16, 16, 31 }, { 24, 21, 13 }, { 31, 8, 8 } };
        const int ci = (itx_color >= 0 && itx_color < 4) ? itx_color : 0;
        const Uint8 first = (Uint8)itx_nearest(cols[ci][0], cols[ci][1], cols[ci][2]);
        h = n * 10 + (n > 1 ? 6 : 0);
        y = (LCD_H - h) / 2;
        if (y < 2) y = 2;
        for (i = 0; i < n; i++) {
            itx_text(y, lines[i], i == 0 ? first : white);
            y += 10;
            if (i == 0 && n > 1) { /* raya bajo el nombre */
                int x;
                for (x = 40; x < LCD_W - 40; x++)
                    top_buf[(y + 1) * LCD_W + x] = first;
                y += 6;
            }
        }
    }
    return 1;
}

/* NDS-TECLADO: teclado en pantalla (tactil) para escribir nombres; dibuja en top_buf y lo copia a la tactil */
static void fnt_char(Uint8 *buf, int px, int py, unsigned char ch, Uint8 col)
{
    const ConsoleFont *f = &consoleGetDefault()->font;
    const int idx = (int)ch - (int)f->asciiOffset;
    int x, y;
    if (idx < 0 || idx >= (int)f->numChars || px < 0 || px > LCD_W - 8 || py < 0 || py > LCD_H - 8)
        return;
    for (y = 0; y < 8; y++) {
        Uint8 *dd = buf + (py + y) * LCD_W + px;
        for (x = 0; x < 8; x++) {
            int on;
            if (f->bpp == 1)
                on = (((const u8 *)f->gfx)[idx * 8 + y] >> x) & 1;
            else if (f->bpp == 4) {
                const u8 bb = ((const u8 *)f->gfx)[idx * 32 + y * 4 + (x >> 1)];
                on = (x & 1) ? (bb >> 4) : (bb & 15);
            } else
                on = ((const u8 *)f->gfx)[idx * 64 + y * 8 + x];
            if (on)
                dd[x] = col;
        }
    }
}
static void fnt_text(int x, int y, const char *t, Uint8 col)
{
    for (; *t; t++, x += 8)
        fnt_char(top_buf, x, y, (unsigned char)*t, col);
}

/* ---------- NDS-TIENDATXT: tiendas con la letra 8x8 a toda la pantalla del juego ---------- */
#define ST_LINES 24
typedef struct { char t[96]; Uint8 col, align, ind, sel, div; int price; } StLine;
static StLine st_l[ST_LINES];
static char st_gold[48];
static int st_cur = -1, st_full, st_scroll;
static short st_y[ST_LINES], st_h[ST_LINES];
void nds_store_begin(int cur, int full, int scroll)
{
    int i;
    for (i = 0; i < ST_LINES; i++) {
        st_l[i].t[0] = 0; st_l[i].div = 0; st_l[i].sel = 0; st_l[i].price = 0;
    }
    st_gold[0] = 0;
    st_cur = cur; st_full = full; st_scroll = scroll;
}
void nds_store_line(int line, const char *text, int color, int align, int indent, int price, int selectable, int divider)
{
    StLine *l;
    if (line < 0 || line >= ST_LINES)
        return;
    l = &st_l[line];
    nds_ascii(l->t, text ? text : "", sizeof l->t);
    l->col = (Uint8)(color & 3); l->align = (Uint8)align; l->ind = (Uint8)(indent > 0);
    l->price = price; l->sel = (Uint8)selectable; l->div = (Uint8)divider;
}
void nds_store_gold(const char *text) { nds_ascii(st_gold, text ? text : "", sizeof st_gold); }
void nds_store_end(void) { st_live = 3; }

static int st_wrap(const char *s, int maxc, char rows[][34], int maxr)
{
    int n = 0;
    if (maxc > 32) maxc = 32;
    while (*s == ' ') s++;
    while (*s && n < maxr) {
        const int len = (int)strlen(s);
        int cut;
        if (len <= maxc) { memcpy(rows[n], s, (size_t)len); rows[n][len] = 0; n++; break; }
        cut = maxc;
        while (cut > 0 && s[cut] != ' ') cut--;
        if (cut <= 0) cut = maxc;
        memcpy(rows[n], s, (size_t)cut); rows[n][cut] = 0; n++;
        s += cut;
        while (*s == ' ') s++;
    }
    return n;
}
static void st_text(int x, int y, const char *t, Uint8 col)
{
    for (; *t; t++, x += 8)
        fnt_char(comp_buf, x, y, (unsigned char)*t, col);
}
#define ST_RIGHT (LCD_W - 4 - (st_scroll ? 12 : 0))
static int st_layout_line(const StLine *l, char rows[][34], int *mc)
{
    const int x0 = 4 + (l->ind ? 8 : 0);
    int n;
    *mc = (ST_RIGHT - x0) / 8;
    n = st_wrap(l->t, *mc, rows, 4);
    if (n > 0 && l->price > 0) {
        char pr[16];
        const int pl = snprintf(pr, sizeof pr, "%d", l->price);
        if ((int)strlen(rows[n - 1]) + 1 + pl > *mc && n < 4)
            n++, rows[n - 1][0] = 0;
    }
    return n;
}
static void st_compose(void)
{
    static int ok;
    static unsigned tick;
    static Uint8 cc[4], cSel, cEdge;
    char rows[4][34];
    int i, x, y, total = 0, gap, mc;
    const int right = ST_RIGHT;
    st_live--;
    if (!ok || ++tick >= 120) {
        dark_build();
        cc[0] = (Uint8)vk_near(31, 31, 31); cc[1] = (Uint8)vk_near(15, 17, 31);
        cc[2] = (Uint8)vk_near(31, 9, 9); cc[3] = (Uint8)vk_near(28, 23, 12);
        cSel = (Uint8)vk_near(10, 7, 3); cEdge = (Uint8)vk_near(20, 16, 9);
        tick = 0; ok = 1;
    }
    for (i = 0; i < LCD_W * LCD_H; i++) /* fondo: el mundo oscurecido */
        comp_buf[i] = dark_lut[comp_buf[i]];
    for (gap = 5; gap >= 2; gap -= 3) { /* alturas de cada linea; si no cabe, huecos mas pequenos */
        total = 3;
        for (i = 0; i < ST_LINES; i++) {
            const StLine *l = &st_l[i];
            int h;
            if (l->div) h = gap + 1;
            else if (!l->t[0]) h = gap;
            else h = st_layout_line(l, rows, &mc) * 9;
            st_y[i] = (short)total; st_h[i] = (short)h;
            total += h;
        }
        if (total <= LCD_H - 12) break;
    }
    for (i = 0; i < ST_LINES; i++) {
        const StLine *l = &st_l[i];
        const int yy = st_y[i];
        if (yy + 8 > LCD_H - 11) { st_h[i] = 0; continue; } /* no cabe: no se dibuja ni se puede tocar */
        if (l->div) {
            for (x = 8; x < right - 4; x++)
                comp_buf[(yy + st_h[i] / 2) * LCD_W + x] = cEdge;
            continue;
        }
        if (!l->t[0])
            continue;
        {
            const int x0 = 4 + (l->ind ? 8 : 0);
            const int n = st_layout_line(l, rows, &mc);
            const Uint8 col = cc[l->col & 3];
            int k;
            if (i == st_cur)
                for (y = yy; y < yy + st_h[i] && y < LCD_H; y++)
                    memset(comp_buf + y * LCD_W + 2, cSel, (size_t)(right - 1));
            for (k = 0; k < n; k++) {
                const int len = (int)strlen(rows[k]);
                int xx = x0;
                if (l->align == 1) xx = x0 + ((right - x0) - len * 8) / 2;
                else if (l->align == 2) xx = right - len * 8;
                st_text(xx, yy + 1 + k * 9, rows[k], col);
            }
            if (n > 0 && l->price > 0) {
                char pr[16];
                const int pl = snprintf(pr, sizeof pr, "%d", l->price);
                st_text(right - pl * 8, yy + 1 + (n - 1) * 9, pr, col);
            }
        }
    }
    if (st_gold[0]) /* oro abajo a la derecha */
        st_text(right - (int)strlen(st_gold) * 8, LCD_H - 10, st_gold, cc[3]);
    if (st_scroll) { /* flechas de desplazamiento a la derecha */
        st_text(LCD_W - 12, 4, "^", cc[3]);
        st_text(LCD_W - 12, LCD_H - 20, "v", cc[3]);
        for (y = 14; y < LCD_H - 22; y++)
            comp_buf[y * LCD_W + LCD_W - 8] = cEdge;
    }
    for (x = 0; x < LCD_W; x++) { comp_buf[x] = cEdge; comp_buf[(LCD_H - 1) * LCD_W + x] = cEdge; }
    for (y = 0; y < LCD_H; y++) { comp_buf[y * LCD_W] = cEdge; comp_buf[y * LCD_W + LCD_W - 1] = cEdge; }
}
static int st_touch(u16 *px, u16 *py)
{
    const int x = *px, y = *py;
    int i, hit = -1;
    if (st_live <= 0)
        return 0;
    if (st_scroll && x >= LCD_W - 16) { /* flechas: linea 4 = subir, linea 20 = bajar */
        hit = y < LCD_H / 2 ? 4 : 20;
        *px = (u16)(UI_X0 + 606);
    } else {
        for (i = 0; i < ST_LINES; i++)
            if (st_h[i] > 0 && y >= st_y[i] && y < st_y[i] + st_h[i]) { hit = i; break; }
        if (hit < 0)
            hit = 3; /* fuera de las lineas: una linea que no hace nada */
        *px = (u16)(UI_X0 + (st_full ? 320 : 480));
    }
    *py = (u16)(32 + hit * 12 + 6);
    return 1;
}
static int vk_near(int r, int g, int b)
{
    int best = 0, bd = 1 << 30, i;
    for (i = 0; i < 256; i++) {
        const u16 cc = BG_PALETTE[i];
        const int dr = (cc & 31) - r, dg = ((cc >> 5) & 31) - g, db = ((cc >> 10) & 31) - b;
        const int dist = dr * dr + dg * dg + db * db;
        if (dist < bd) { bd = dist; best = i; }
    }
    return best;
}
static void belt_draw(void) /* NDS-CINTURON */
{
    static int col_txt = -1, col_bg = -1;
    static unsigned col_tick;
    const unsigned char *pan = ui_pix + 352 * ui_pitch; /* panel principal en (448,352) */
    int i, y;
    if (col_txt < 0 || ++col_tick >= 120) { /* la paleta cambia entre niveles */
        col_txt = vk_near(31, 26, 12);
        col_bg = vk_near(2, 2, 2);
        col_tick = 0;
    }
    for (y = BL_Y - 14; y < BL_Y + 4 * BL_S + 4; y++)
        memset(comp_buf + y * LCD_W + BL_X - 6, col_bg, 2 * BL_S + 12);
    for (i = 0; i < 8; i++) {
        const int dx = BL_X + (i & 1) * BL_S, dy = BL_Y + (i >> 1) * BL_S;
        const unsigned char *src = pan + 5 * ui_pitch + 205 + i * BL_S;
        for (y = 0; y < BL_S; y++)
            memcpy(comp_buf + (dy + y) * LCD_W + dx, src + y * ui_pitch, BL_S);
    }
    { /* NDS-IDIOMA */
        const char *lbl = nds_lang_es ? "CINTURON" : "  BELT";
        for (i = 0; lbl[i]; i++)
            fnt_char(comp_buf, BL_X - 3 + i * 8, BL_Y - 11, (unsigned char)lbl[i], (Uint8)col_txt);
    }
}
static void vk_fill(int x, int y, int w, int h, Uint8 c)
{
    int j;
    for (j = 0; j < h; j++)
        memset(top_buf + (y + j) * LCD_W + x, c, (size_t)w);
}
static void vk_frame(int x, int y, int w, int h, Uint8 c)
{
    int j;
    memset(top_buf + y * LCD_W + x, c, (size_t)w);
    memset(top_buf + (y + h - 1) * LCD_W + x, c, (size_t)w);
    for (j = 0; j < h; j++) {
        top_buf[(y + j) * LCD_W + x] = c;
        top_buf[(y + j) * LCD_W + x + w - 1] = c;
    }
}
typedef struct { short x, y, w, h; char code; } VKey; /* code: letra, o 1 mayus, 2 espacio, 3 borrar, 4 ok */
static int vk_build(VKey *k)
{
    static const char *rows[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM-" };
    static const short x0[4] = { 3, 3, 15, 40 };
    int n = 0, r, i;
    for (r = 0; r < 4; r++)
        for (i = 0; rows[r][i]; i++) {
            k[n].x = (short)(x0[r] + i * 25); k[n].y = (short)(44 + r * 28);
            k[n].w = 24; k[n].h = 26; k[n].code = rows[r][i]; n++;
        }
    k[n].x = 3;   k[n].y = 158; k[n].w = 58; k[n].h = 30; k[n].code = 1; n++;
    k[n].x = 64;  k[n].y = 158; k[n].w = 76; k[n].h = 30; k[n].code = 2; n++;
    k[n].x = 143; k[n].y = 158; k[n].w = 56; k[n].h = 30; k[n].code = 3; n++;
    k[n].x = 202; k[n].y = 158; k[n].w = 51; k[n].h = 30; k[n].code = 4; n++;
    return n;
}
void nds_vkbd_input(const char *hint, char *buf, int maxlen)
{
    VKey k[48];
    const int n = vk_build(k);
    int len, shift, pressed = -1, dirty = 1, i;
    const Uint8 cBg = (Uint8)vk_near(0, 0, 0), cKey = (Uint8)vk_near(7, 6, 6), cEdge = (Uint8)vk_near(20, 16, 9);
    const Uint8 cTxt = (Uint8)vk_near(31, 31, 31), cGold = (Uint8)vk_near(28, 23, 12);
    if (maxlen > 60) maxlen = 60;
    if (maxlen < 1) maxlen = 1;
    len = (int)strlen(buf);
    if (len > maxlen) { len = maxlen; buf[len] = 0; }
    shift = (len == 0);
    lcdMainOnBottom(); /* NDS-TECLADO2: el teclado siempre en la tactil */
    while (dmaBusy(2))
        ;
    do { swiWaitForVBlank(); scanKeys(); } while (keysHeld() & (KEY_A | KEY_B | KEY_START | KEY_TOUCH));
    for (;;) {
        u32 down, held;
        int act = 0;
        if (dirty) {
            char line[64];
            memset(top_buf, cBg, sizeof top_buf);
            if (hint && *hint) {
                const int hl = (int)strlen(hint) > 31 ? 31 : (int)strlen(hint);
                char h2[32];
                memcpy(h2, hint, (size_t)hl); h2[hl] = 0;
                fnt_text((LCD_W - hl * 8) / 2, 4, h2, cGold);
            }
            vk_frame(6, 16, 244, 22, cEdge);
            snprintf(line, sizeof line, "%s_", buf);
            { const int ll = (int)strlen(line); fnt_text(ll > 29 ? 12 - (ll - 29) * 8 : 12, 23, line, cTxt); }
            for (i = 0; i < n; i++) {
                const int act_on = (i == pressed) || (k[i].code == 1 && shift);
                const char *lab;
                char one[2];
                vk_fill(k[i].x, k[i].y, k[i].w, k[i].h, act_on ? cGold : cKey);
                vk_frame(k[i].x, k[i].y, k[i].w, k[i].h, cEdge);
                switch (k[i].code) {
                case 1: lab = nds_lang_es ? "MAYUS" : "SHIFT"; break; /* NDS-IDIOMA */
                case 2: lab = nds_lang_es ? "ESPACIO" : "SPACE"; break;
                case 3: lab = nds_lang_es ? "BORRAR" : "DELETE"; break;
                case 4: lab = "OK"; break;
                default:
                    one[0] = (!shift && k[i].code >= 'A' && k[i].code <= 'Z') ? (char)(k[i].code + 32) : k[i].code;
                    one[1] = 0; lab = one; break;
                }
                fnt_text(k[i].x + (k[i].w - (int)strlen(lab) * 8) / 2, k[i].y + (k[i].h - 8) / 2, lab, act_on ? cBg : cTxt);
            }
            DC_FlushRange(top_buf, sizeof top_buf);
            dmaCopyWords(3, top_buf, video_vram, sizeof top_buf);
            dmaCopyWords(3, top_buf, (Uint8 *)video_vram + 0x10000, sizeof top_buf);
            dirty = 0;
        }
        swiWaitForVBlank();
        nds_audio_update();
        scanKeys();
        down = keysDown();
        held = keysHeld();
        if (down & KEY_TOUCH) {
            touchPosition t;
            touchRead(&t);
            for (i = 0; i < n; i++)
                if (t.px >= k[i].x && t.px < k[i].x + k[i].w && t.py >= k[i].y && t.py < k[i].y + k[i].h) {
                    pressed = i; act = k[i].code; break;
                }
        }
        if (pressed >= 0 && !(held & KEY_TOUCH)) { pressed = -1; dirty = 1; }
        if (down & KEY_B) act = 3;
        if (down & (KEY_A | KEY_START)) act = 4;
        if (down & KEY_SELECT) act = 1;
        if (!act)
            continue;
        dirty = 1;
        if (act == 4)
            break;
        if (act == 1)
            shift = !shift;
        else if (act == 3) {
            if (len > 0) buf[--len] = 0;
            if (len == 0) shift = 1;
        } else {
            char c = (act == 2) ? ' ' : (char)act;
            const int letter = (c >= 'A' && c <= 'Z');
            if (letter && !shift) c = (char)(c + 32);
            if (len < maxlen) { buf[len++] = c; buf[len] = 0; }
            if (letter && shift) shift = 0;
        }
    }
    do { swiWaitForVBlank(); scanKeys(); } while (keysHeld() & (KEY_A | KEY_START | KEY_TOUCH));
    memset(top_buf, 0, sizeof top_buf);
    nds_apply_swap(); /* NDS-TECLADO2: vuelve la eleccion de pantallas del jugador */
}

static inline void hud_touch(const void *p, unsigned n); /* NDS-HUD5b */
/* NDS-HUD5: texto del cuadro del HUD (enemigos, NPC, objetos) y piezas del panel clasico */
static char hinfo_buf[512];
static int hinfo_color;
/* NDS-NIVEL: aviso de nivel (3 s) y boton "+" de puntos sin repartir */
static Uint32 lvl_until;
static int lvl_num, ui_statpts;
static volatile int nds_chr_req;
void nds_levelup(int lvl) { lvl_num = lvl; lvl_until = (SDL_GetTicks() + 3000u) | 1u; }
void nds_ui_statpts(int on) { ui_statpts = on; }
int nds_take_chrpanel(void) { const int r = nds_chr_req; nds_chr_req = 0; return r; }
static int lvl_active(void)
{
    if (lvl_until && (int)(SDL_GetTicks() - lvl_until) >= 0)
        lvl_until = 0;
    return lvl_until != 0;
}
void nds_info_text(const char *s, int color)
{
    if (!s || !*s) { hinfo_buf[0] = 0; return; }
    nds_ascii(hinfo_buf, s, sizeof hinfo_buf); /* NDS-TILDES */
    hinfo_color = color;
}
static int hud_wrap(const char *p, char lines[][20], int maxl) /* NDS-HUD5e: 18 letras */
{
    int n = 0;
    while (n < maxl && *p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len > 0 && n < maxl) {
            size_t take = len > 18 ? 18 : len;
            if (len > 18) { size_t k = take; while (k > 0 && p[k] != ' ') k--; if (k > 0) take = k; }
            memcpy(lines[n], p, take);
            lines[n][take] = 0;
            n++;
            p += take; len -= take;
            while (len > 0 && *p == ' ') { p++; len--; }
        }
        if (!e) break;
        p = e + 1;
    }
    return n;
}
static void hud_piece(int px, int dx) /* frasco con su estatua: 104x141 del panel a 64x88 */
{
    int x, y;
    const int dw = 64, dh = 88, dy = 99;
    const unsigned stx = ((unsigned)HUD_FL_SW << 16) / (unsigned)dw;
    for (y = 0; y < dh; y++) {
        const int sy = y * HUD_FL_SH / dh;
        const unsigned char *src = hud_pan + (sy - 13) * hud_pitch + px + HUD_FL_SX;
        Uint8 *d = top_buf + (dy + y) * LCD_W + dx;
        unsigned fx = 0;
        hud_touch(d, (unsigned)dw);
        for (x = 0; x < dw; x++, fx += stx) {
            const int sx = (int)(fx >> 16);
            const int rel = sx + HUD_FL_SX;
            const int ax = px + HUD_FL_SX + sx; /* NDS-HUD5d: columna y fila en el panel */
            const int ay = sy - 13;
            if (sy < 13 && (rel < 11 || rel >= 73))
                d[x] = 0;
            else if (ay >= 40 && ((px < 300 && ax >= 175) || (px >= 300 && ax <= 467))) /* NDS-HUD5g: zona del cuadro original: reflejo del otro lado del frasco */
                d[x] = hud_pan[ay * hud_pitch + (2 * (px + HUD_FL_SX) + HUD_FL_SW - ax)];
            else
                d[x] = src[sx];
        }
    }
}
static int hud_green_idx = -1, hud_greenhi_idx = -1; /* NDS-XPBAR: verde y verde claro de la experiencia (opcion C) */
static unsigned hud_green_n;
static Uint8 hud_green_hi;
static Uint8 hud_green(int avoid) /* NDS-HUDFIX: colores prestados mas seguros */
{
    static unsigned char seen[256]; /* colores que han salido alguna vez en el HUD: no se tocan */
    static int real; /* 1 = no queda color libre: se usa un verde real de la paleta, sin cambiarla */
    if (hud_green_idx < 0 || (++hud_green_n & 7) == 0) {
        static unsigned short cnt[256];
        const int olda = hud_green_idx, oldb = hud_greenhi_idx, oldreal = real;
        int i, a = -1, b = -1;
        memset(cnt, 0, sizeof cnt);
        for (i = 0; i < LCD_W * 188; i++)
            cnt[top_buf[i]]++;
        for (i = 0; i < 256; i++)
            if (cnt[i])
                seen[i] = 1;
        seen[0] = 1;
        if (avoid > 0 && avoid < 256)
            seen[avoid] = 1;
        if (!oldreal && olda > 0 && !seen[olda])
            a = olda; /* si siguen libres, se mantienen */
        if (!oldreal && oldb > 0 && !seen[oldb] && oldb != a)
            b = oldb;
        for (i = 255; i > 0 && (a < 0 || b < 0); i--) {
            if (seen[i] || i == a || i == b)
                continue;
            if (a < 0) a = i; else b = i;
        }
        real = 0;
        if (a < 0) { /* no queda ninguno libre */
            a = itx_nearest(6, 26, 10);
            b = itx_nearest(13, 31, 17);
            real = 1;
        } else if (b < 0)
            b = a;
        if (!oldreal) { /* devolver su color original a los que se sueltan */
            if (olda > 0 && olda != a && olda != b)
                BG_PALETTE_SUB[olda] = BG_PALETTE[olda];
            if (oldb > 0 && oldb != a && oldb != b)
                BG_PALETTE_SUB[oldb] = BG_PALETTE[oldb];
        }
        hud_green_idx = a;
        hud_greenhi_idx = b;
    }
    if (!real) {
        BG_PALETTE_SUB[hud_green_idx] = RGB15(6, 26, 10);
        if (hud_greenhi_idx != hud_green_idx)
            BG_PALETTE_SUB[hud_greenhi_idx] = RGB15(13, 31, 17);
    }
    hud_green_hi = (Uint8)hud_greenhi_idx;
    return (Uint8)hud_green_idx;
}
/* NDS-HUD6: panel clasico entero de un solo escalado (columnas 88-560, 13 filas sobre el panel + 128) a 256x92 */
static void hud_full(void)
{
    int x, y;
    const unsigned stx = (472u << 16) / 256u;
    for (y = 111; y < 111; y++) /* NDS-HUD6b/6c/6d (ya sin hueco): misma escala en ancho y alto (bolas redondas): 77 filas, hueco negro arriba */
        memset(top_buf + y * LCD_W, 0, LCD_W);
    for (y = 0; y < 77; y++) {
        const int ay = y * 141 / 77 - 13;
        const unsigned char *src = hud_pan + ay * hud_pitch + 88;
        Uint8 *d = top_buf + (111 + y) * LCD_W;
        unsigned fx = 0;
        hud_touch(d, LCD_W);
        for (x = 0; x < LCD_W; x++, fx += stx) {
            const int ax = 88 + (int)(fx >> 16);
            if (ay < 0 && !((ax >= 107 && ax < 169) || (ax >= 475 && ax < 537)))
                d[x] = 0; /* sobre el panel solo la punta de las bolas */
            else
                d[x] = src[fx >> 16];
        }
    }
}
#define HUD6_X0 52  /* interior del cuadro de texto (pantalla de arriba) */
#define HUD6_X1 201
#define HUD6_Y0 146 /* NDS-HUD6b */
#define HUD6_Y1 173
static void hud_text6(void)
{
    char lines[4][20];
    int j, n, i;
    for (j = HUD6_Y0; j <= HUD6_Y1; j++)
        memset(top_buf + j * LCD_W + HUD6_X0, 0, HUD6_X1 - HUD6_X0 + 1);
    const char *htxt = hinfo_buf; /* NDS-NIVEL: durante el aviso, "NIVEL N" en dorado */
    int hcol = hinfo_color;
    char lvbuf[24];
    if (lvl_active()) {
        snprintf(lvbuf, sizeof lvbuf, nds_lang_es ? "NIVEL %d" : "LEVEL %d", lvl_num); /* NDS-IDIOMA */
        htxt = lvbuf;
        hcol = 2;
    }
    n = hud_wrap(htxt, lines, 3); /* NDS-HUD6b: 3 lineas */
    if (n > 0) {
        static const unsigned char cols[4][3] = { { 31, 31, 31 }, { 16, 16, 31 }, { 24, 21, 13 }, { 31, 8, 8 } };
        const int ci = (hcol >= 0 && hcol < 4) ? hcol : 0;
        const Uint8 white = (Uint8)itx_nearest(31, 31, 31);
        const Uint8 first = (Uint8)itx_nearest(cols[ci][0], cols[ci][1], cols[ci][2]);
        int y = HUD6_Y0 + 2 + (3 - n) * 8 / 2;
        for (i = 0; i < n; i++, y += 8) {
            const int len = (int)strlen(lines[i]);
            int k, x = (HUD6_X0 + HUD6_X1 + 1) / 2 - len * 4;
            for (k = 0; k < len; k++, x += 8)
                itx_char(x, y, (unsigned char)lines[i][k], i == 0 ? first : white);
        }
    }
}
static void hud_textbox(void)
{
    char lines[6][20];
    int j, n, i;
    const Uint8 edge = (Uint8)itx_nearest(22, 18, 9);
    for (j = 120; j <= 186; j++) /* NDS-HUD5g: cuadro 66-189 x 120-186 (solo el centro) */
        memset(top_buf + j * LCD_W + 66, 0, 124);
    memset(top_buf + 120 * LCD_W + 66, edge, 124);
    memset(top_buf + 186 * LCD_W + 66, edge, 124);
    for (j = 120; j <= 186; j++) {
        top_buf[j * LCD_W + 66] = edge;
        top_buf[j * LCD_W + 189] = edge;
    }
    n = hud_wrap(hinfo_buf, lines, 6);
    if (n > 0) {
        static const unsigned char cols[4][3] = { { 31, 31, 31 }, { 16, 16, 31 }, { 24, 21, 13 }, { 31, 8, 8 } };
        const int ci = (hinfo_color >= 0 && hinfo_color < 4) ? hinfo_color : 0;
        const Uint8 white = (Uint8)itx_nearest(31, 31, 31);
        const Uint8 first = (Uint8)itx_nearest(cols[ci][0], cols[ci][1], cols[ci][2]);
        int y = 126 + (6 - n) * 9 / 2;
        for (i = 0; i < n; i++, y += 9) {
            const int len = (int)strlen(lines[i]);
            int k, x = 128 - len * 4;
            for (k = 0; k < len; k++, x += 8)
                itx_char(x, y, (unsigned char)lines[i][k], i == 0 ? first : white);
        }
    }
}

void nds_hud_frame(const unsigned char *panel, int pitch, int xp_permille)
{
    hud_pan = panel;
    hud_pitch = pitch;
    hud_xp = xp_permille < 0 ? 0 : (xp_permille > 1000 ? 1000 : xp_permille);
}

/* NDS-RAPIDO7: traer a la cache las lineas de destino antes de escribir bytes en ellas */
static inline void hud_touch(const void *p, unsigned n)
{
    unsigned a = (unsigned)p & ~31u;
    const unsigned e = (unsigned)p + n - 1;
    for (; a <= e; a += 32)
        (void)*(volatile const Uint8 *)a;
}

static void hud_blit(const unsigned char *src, int spitch, int sw, int sh, int dx, int dy, int dw, int dh)
{
    int x, y; /* NDS-RAPIDO1: paso en coma fija, sin dividir en cada pixel */
    const unsigned stx = ((unsigned)sw << 16) / (unsigned)dw;
    for (y = 0; y < dh; y++) {
        const unsigned char *s = src + (y * sh / dh) * spitch;
        Uint8 *d = top_buf + (dy + y) * LCD_W + dx;
        unsigned fx = 0;
        hud_touch(d, (unsigned)dw); /* NDS-RAPIDO7 */
        for (x = 0; x < dw; x++, fx += stx)
            d[x] = s[fx >> 16];
    }
}

static void hud_flask(int px, int dx)
{
    int x, y; /* NDS-RAPIDO7: reduccion directa, sin bufer intermedio (mismo muestreo que antes) */
    const int dy = HUD_XP_Y + HUD_XP_H - HUD_FLASK_H;
    const unsigned stx = ((unsigned)HUD_FL_SW << 16) / (unsigned)HUD_FLASK_W;
    (void)flask_tmp;
    for (y = 0; y < HUD_FLASK_H; y++) {
        const int sy = y * HUD_FL_SH / HUD_FLASK_H; /* fila 0 = 13 filas por encima del panel */
        const unsigned char *src = hud_pan + (sy - 13) * hud_pitch + px + HUD_FL_SX;
        Uint8 *d = top_buf + (dy + y) * LCD_W + dx;
        unsigned fx = 0;
        hud_touch(d, HUD_FLASK_W);
        for (x = 0; x < HUD_FLASK_W; x++, fx += stx) {
            const int sx = (int)(fx >> 16);
            const int rel = sx + HUD_FL_SX; /* columna relativa al frasco */
            d[x] = (sy < 13 && (rel < 11 || rel >= 73)) ? 0 : src[sx];
        }
    }
}

static Uint8 chr_bg[LCD_W * LCD_H]; /* NDS-PERSONAJE7b: fondo limpio del panel del personaje, ya al 80% */
static int chr_bg_ok;
static Uint8 chr_hdr[320 * 111]; /* NDS-PERSONAJE7c: fondo limpio de la cabecera a tamano completo */
void nds_chr_bg(const unsigned char *p, int pitch, int w, int h)
{
    int x, y;
    if (!p || w < 320 || h < 240)
        return;
    for (y = 0; y < LCD_H; y++) {
        const unsigned char *src = p + (y * 5 / 4) * pitch;
        Uint8 *d = chr_bg + y * LCD_W;
        for (x = 0; x < LCD_W; x++)
            d[x] = src[x * 5 / 4];
    }
    for (y = 0; y < 111; y++) /* NDS-PERSONAJE7c */
        memcpy(chr_hdr + y * 320, p + y * pitch, 320);
    chr_bg_ok = 1;
}
static inline int hud_mir(int v, int n) /* NDS-PERSONAJE6: repeticion en espejo */
{
    v %= 2 * n;
    return v < n ? v : 2 * n - 1 - v;
}
static void nds_hud_compose(void)
{
    int x, y, white = 240, dark = 240;
    { /* NDS-RAPIDO5: limpiar solo la primera vez (las zonas usadas se sobrescriben siempre) */
        static int top_clean;
        if (!top_clean) {
            memset(top_buf, 0, sizeof top_buf);
            top_clean = 1;
        }
    }
    while (dmaBusy(2)) /* NDS-RAPIDO5: esperar a que termine la copia anterior */
        ;
    if (itx_compose()) /* NDS-ITEMTXT: con descripcion de objeto, arriba solo el texto */
        return;
    /* NDS-HUD3: minimapa arriba (256x96 de la esquina de la superficie de interfaz) */
    {
        const unsigned char *map = hud_pan - 352 * hud_pitch - 448;
        if (ui_mode == 1 && (ui_wide & 1)) { /* NDS-PERSONAJE5: arriba todo piedra con la cabecera del panel (nombre, clase, nivel, experiencia) */
            const unsigned char *cp = hud_pan - 352 * hud_pitch;
            for (y = 0; y < LCD_H; y++) { /* NDS-PERSONAJE6: piedra en espejo + bordes + 3 filas de recuadros centradas */
                int sy = -1, band = 1;
                Uint8 *d = top_buf + y * LCD_W;
                if (y < 8)
                    sy = y * 5 / 4;                   /* borde de arriba */
                else if (y >= 31 && y < 56)
                    sy = 11 + (y - 31) * 5 / 4;       /* nombre y clase */
                else if (y >= 85 && y < 109)
                    sy = 49 + (y - 85) * 5 / 4;       /* nivel y experiencia */
                else if (y >= 139 && y < 162)
                    sy = 79 + (y - 139) * 5 / 4;      /* siguiente nivel */
                else
                    band = 0;
                hud_touch(d, LCD_W);
                if (!band && chr_bg_ok) { /* NDS-PERSONAJE7b: la misma piedra que abajo */
                    memcpy(d, chr_bg + y * LCD_W, LCD_W);
                    continue;
                }
                for (x = 0; x < LCD_W; x++) {
                    int sx, ry;
                    const unsigned char *s0, *s1;
                    Uint8 a, b, c, e;
                    if (band) {
                        sx = x * 5 / 4;
                        ry = sy;
                    } else if (x < 7 || x >= LCD_W - 7) { /* bordes laterales del panel */
                        sx = x * 5 / 4;
                        ry = 120 + hud_mir(y * 5 / 4, 180);
                    } else {                               /* piedra limpia repetida en espejo */
                        sx = 14 + hud_mir(x * 5 / 4, 100);
                        ry = 82 + hud_mir(y * 5 / 4, 22);
                    }
                    s0 = cp + ry * hud_pitch;
                    s1 = s0 + hud_pitch;
                    if (band && chr_bg_ok && ry + 1 < 111) { /* NDS-PERSONAJE7c: si es piedra, el fondo continuo (sin cortes) */
                        const Uint8 *h0 = chr_hdr + ry * 320, *h1 = h0 + 320;
                        if (s0[sx] == h0[sx] && s0[sx + 1] == h0[sx + 1] && s1[sx] == h1[sx] && s1[sx + 1] == h1[sx + 1]) {
                            d[x] = chr_bg[y * LCD_W + x];
                            continue;
                        }
                    }
                    a = s0[sx]; b = s0[sx + 1]; c = s1[sx]; e = s1[sx + 1];
                    if (pal_lum[b] > pal_lum[a]) a = b;
                    if (pal_lum[c] > pal_lum[a]) a = c;
                    if (pal_lum[e] > pal_lum[a]) a = e;
                    d[x] = a;
                }
            }
            return; /* sin HUD mientras el panel del personaje esta abierto */
        } else {
        for (y = 0; y < 111; y++) /* NDS-HUD6d: minimapa 256x111 hasta encima de las bolas */
            memcpy(top_buf + y * LCD_W, map + y * hud_pitch, LCD_W);
        { /* NDS-DURABILIDAD: iconos de equipo (188x40 en x 260 de la interfaz) a la mitad, arriba a la derecha */
            const unsigned char *ic = map + 260;
            for (y = 0; y < 20; y++) {
                const unsigned char *s0 = ic + (2 * y) * hud_pitch;
                Uint8 *d = top_buf + (2 + y) * LCD_W + (LCD_W - 96);
                for (x = 0; x < 94; x++) {
                    const Uint8 v = s0[2 * x];
                    if (v)
                        d[x] = v;
                }
            }
        }
        }
    }
    /* NDS-HUD6: mitad de abajo = panel clasico de un solo escalado + texto 8x8 en el cuadro */
    hud_full();
    hud_text6();
    for (x = 240; x < 256; x++) {     /* gris oscuro para la parte vacia de la experiencia */
        if (pal_lum[x] > pal_lum[white])
            white = x;
        if (pal_lum[x] < pal_lum[dark])
            dark = x;
    }
    {
        const Uint8 g = hud_green(dark); /* NDS-HUDFIX */
        const int fill = LCD_W * hud_xp / 1000;
        for (y = 188; y < 192; y++) {
            Uint8 *d = top_buf + y * LCD_W;
            hud_touch(d, LCD_W);
            const int blink = lvl_active() && ((SDL_GetTicks() / 200u) & 1u); /* NDS-NIVEL: la barra parpadea */
            for (x = 0; x < LCD_W; x++) /* NDS-XPBAR: 8 tramos (rayas negras), fila de arriba mas clara */
                d[x] = (Uint8)(((x & 31) == 31 && x < LCD_W - 1) ? 0 : (blink ? hud_green_hi : (x < fill ? (y == 188 ? hud_green_hi : g) : dark)));
        }
    }
}

#pragma GCC pop_options /* NDS-RAPIDO1 */

#include <malloc.h> /* NDS-MEM */
/* ---------- NDS-MEMTRK: rastreador de memoria ---------- */
#define NDS_MEMTRACK 0 /* 0 = sin rastreo (solo pasa las llamadas)  NDS-MP12: apagado */
void *__real_malloc(size_t n);
void __real_free(void *p);
void *__real_realloc(void *p, size_t n);
void *__real_calloc(size_t a, size_t b);
#define MT_MAGIC 0xD1AB10C5u
#define MT_N 1024
static struct { unsigned ra, bytes, count; } mt_b[MT_N];
static unsigned mt_total, mt_blocks, mt_lost;

static int mt_slot(unsigned ra)
{
    unsigned h = ((ra >> 1) * 2654435761u) >> 22, i;
    for (i = 0; i < MT_N; i++) {
        unsigned k = (h + i) & (MT_N - 1);
        if (mt_b[k].ra == ra)
            return (int)k;
        if (mt_b[k].ra == 0) {
            mt_b[k].ra = ra;
            return (int)k;
        }
    }
    return -1;
}
static void mt_add(unsigned ra, unsigned n, int sign)
{
    int k = mt_slot(ra);
    if (k < 0) {
        mt_lost += sign > 0 ? n : (unsigned)-(int)n;
        return;
    }
    if (sign > 0) {
        mt_b[k].bytes += n;
        mt_b[k].count++;
        mt_total += n;
        mt_blocks++;
    } else {
        mt_b[k].bytes -= n;
        mt_b[k].count--;
        mt_total -= n;
        mt_blocks--;
    }
}
static u32 *mt_hdr(void *p)
{
    u32 *h = (u32 *)p - 4;
    return (h[2] == MT_MAGIC && h[3] == ~h[1]) ? h : NULL;
}
static void *mt_new(size_t n, unsigned ra)
{
    u32 *h = (u32 *)__real_malloc(n + 16);
    if (!h)
        return NULL;
    h[0] = ra;
    h[1] = (u32)n;
    h[2] = MT_MAGIC;
    h[3] = ~(u32)n;
    mt_add(ra, (unsigned)n, 1);
    return h + 4;
}
/* NDS-MEMNOMBRES: nombre de los bloques grandes (archivos cargados, superficies) */
#define MN_N 160
static struct { const void *p; char name[36]; } mn_t[MN_N];
void nds_mem_name(const void *p, const char *name)
{
    u32 *h;
    int i, k = -1;
    size_t L;
    if (!NDS_MEMTRACK || !p || !name)
        return;
    h = mt_hdr((void *)p);
    if (!h || h[1] < 8192u)
        return;
    for (i = 0; i < MN_N; i++) {
        if (mn_t[i].p == p) {
            k = i;
            break;
        }
        if (k < 0 && !mn_t[i].p)
            k = i;
    }
    if (k < 0)
        return;
    L = strlen(name);
    if (L > sizeof mn_t[k].name - 1)
        name += L - (sizeof mn_t[k].name - 1); /* el final de la ruta es lo que identifica */
    strncpy(mn_t[k].name, name, sizeof mn_t[k].name - 1);
    mn_t[k].name[sizeof mn_t[k].name - 1] = 0;
    mn_t[k].p = p;
}
static void mn_forget(const void *p)
{
    int i;
    for (i = 0; i < MN_N; i++)
        if (mn_t[i].p == p)
            mn_t[i].p = NULL;
}

void *__wrap_malloc(size_t n)
{
#if NDS_MEMTRACK
    return mt_new(n, (unsigned)__builtin_return_address(0));
#else
    return __real_malloc(n);
#endif
}
void __wrap_free(void *p)
{
    u32 *h;
    if (!p)
        return;
    h = NDS_MEMTRACK ? mt_hdr(p) : NULL;
    if (h) {
        if (h[1] >= 8192u)
            mn_forget(p); /* NDS-MEMNOMBRES */
        mt_add(h[0], h[1], -1);
        h[2] = 0;
        __real_free(h);
    } else
        __real_free(p);
}
void *__wrap_realloc(void *p, size_t n)
{
    unsigned ra = (unsigned)__builtin_return_address(0);
    u32 *h, *nh;
    unsigned ora, on;
    if (!NDS_MEMTRACK)
        return __real_realloc(p, n);
    if (!p)
        return mt_new(n, ra);
    h = mt_hdr(p);
    if (!h)
        return __real_realloc(p, n);
    if (n == 0) {
        __wrap_free(p);
        return NULL;
    }
    ora = h[0];
    on = h[1];
    if (on >= 8192u)
        mn_forget(p); /* NDS-MEMNOMBRES */
    nh = (u32 *)__real_realloc(h, n + 16);
    if (!nh)
        return NULL;
    mt_add(ora, on, -1);
    nh[0] = ra;
    nh[1] = (u32)n;
    nh[2] = MT_MAGIC;
    nh[3] = ~(u32)n;
    mt_add(ra, (unsigned)n, 1);
    return nh + 4;
}
void *__wrap_calloc(size_t a, size_t b)
{
    size_t n = a * b;
    void *p;
    if (b != 0 && n / b != a)
        return NULL;
    p = NDS_MEMTRACK ? mt_new(n, (unsigned)__builtin_return_address(0)) : __real_malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}
void nds_mem_retag(void *p, unsigned ra) /* llamado desde operator new */
{
    u32 *h = p ? mt_hdr(p) : NULL;
    if (!h)
        return;
    mt_add(h[0], h[1], -1);
    h[0] = ra;
    mt_add(ra, h[1], 1);
}

static void mt_write(const char *path)
{
    enum { TOP = 40 };
    unsigned idx[TOP], n = 0, i, j;
    struct mallinfo mi = mallinfo();
    const unsigned tot = mt_total, blk = mt_blocks, lost = mt_lost;
    FILE *f;
    for (i = 0; i < MT_N; i++) { /* los TOP mayores (seleccion simple) */
        if (!mt_b[i].ra || !mt_b[i].bytes)
            continue;
        if (n < TOP)
            idx[n++] = i;
        else {
            unsigned m = 0;
            for (j = 1; j < TOP; j++)
                if (mt_b[idx[j]].bytes < mt_b[idx[m]].bytes)
                    m = j;
            if (mt_b[i].bytes > mt_b[idx[m]].bytes)
                idx[m] = i;
        }
    }
    for (i = 0; i < n; i++) /* ordenar de mayor a menor */
        for (j = i + 1; j < n; j++)
            if (mt_b[idx[j]].bytes > mt_b[idx[i]].bytes) {
                unsigned t = idx[i];
                idx[i] = idx[j];
                idx[j] = t;
            }
    {
        unsigned kb[TOP], cnt[TOP], ra[TOP];
        for (i = 0; i < n; i++) { /* copiar antes de abrir el archivo (fopen tambien reserva) */
            kb[i] = mt_b[idx[i]].bytes / 1024u;
            cnt[i] = mt_b[idx[i]].count;
            ra[i] = mt_b[idx[i]].ra;
        }
        enum { NTOP = 40 };
        unsigned nkb[NTOP], nn = 0;
        char nname[NTOP][36];
        for (i = 0; i < MN_N; i++) { /* NDS-MEMNOMBRES: los bloques con nombre, de mayor a menor */
            u32 *hh = mn_t[i].p ? mt_hdr((void *)mn_t[i].p) : NULL;
            unsigned kbv, pos;
            if (!hh)
                continue;
            kbv = hh[1] / 1024u;
            for (pos = 0; pos < nn && nkb[pos] >= kbv; pos++) { }
            if (pos >= NTOP)
                continue;
            if (nn < NTOP)
                nn++;
            for (j = nn - 1; j > pos; j--) {
                nkb[j] = nkb[j - 1];
                memcpy(nname[j], nname[j - 1], 36);
            }
            nkb[pos] = kbv;
            memcpy(nname[pos], mn_t[i].name, 36);
        }
        f = fopen(path, "w");
        if (!f)
            return;
        fprintf(f, "usado %u KB (mallinfo)  rastreado %u KB en %u bloques  sin tabla %u KB\n",
            (unsigned)(mi.uordblks / 1024), tot / 1024u, blk, lost / 1024u);
        fprintf(f, "KB      bloques  direccion\n");
        for (i = 0; i < n; i++)
            fprintf(f, "%6u  %7u  0x%08x\n", kb[i], cnt[i], ra[i]);
        fprintf(f, "\nArchivos y superficies (KB nombre):\n");
        for (i = 0; i < nn; i++)
            fprintf(f, "%6u  %s\n", nkb[i], nname[i]);
        fclose(f);
    }
}
static void nds_mem_tick(void)
{
    static Uint32 last;
    static unsigned peak;
    static int in_game;
    const Uint32 now = SDL_GetTicks();
    if (!NDS_MEMTRACK)
        return;
    if (hud_pan)
        in_game = 1;
    if (in_game && mt_total > peak + 256u * 1024u) {
        peak = mt_total;
        mt_write(nds_r("/devilutionx/memoria_pico.txt"));
    }
    if (hud_pan && ui_mode < 4 && now - last >= 10000u) { /* NDS-MEMNOMBRES: solo en partida */
        last = now;
        mt_write(nds_r("/devilutionx/memoria.txt"));
    }
}
/* ---------- medidor de rendimiento (NDS-PROF) ---------- */
/* 0 logica, 1 mundo (DrawGame), 2 vista (DrawView), 3 minimapa, 4 dibujo total, 5 flip */
static unsigned prof_sum[11], prof_cnt[11], prof_last;
static unsigned prof_mon_total, prof_mon_run; /* NDS-RAPIDO6: monstruos del nivel / procesados */
void nds_prof_mon(unsigned total, unsigned run)
{
    prof_mon_total = total;
    prof_mon_run = run;
} /* NDS-RAPIDO5: 6-10 partes de la logica */
static char prof_txt[7][40] = { "midiendo...", "", "" }; /* NDS-RAPIDO2: 3 medidas + 4 de diagnostico */

/* NDS-RAPIDO2: velocidad de la memoria principal y configuracion de cache del ARM9 (CP15) */
unsigned nds_usec(void);
static u32 diag_buf[8192]; /* 32 KB en memoria principal */
__attribute__((target("arm"), noinline)) static void nds_diag(void)
{
    volatile Uint8 *b8 = (volatile Uint8 *)diag_buf;
    volatile u32 *b32 = (volatile u32 *)diag_buf;
    unsigned t0, t1, t2, t3, t4, sum = 0;
    u32 c1, c2, c3, r[8];
    int i;
    t0 = nds_usec();
    for (i = 0; i < 32768; i++) b8[i] = (Uint8)i;
    t1 = nds_usec();
    for (i = 0; i < 8192; i++) b32[i] = (u32)i;
    t2 = nds_usec();
    for (i = 0; i < 32768; i++) sum += b8[i];
    t3 = nds_usec();
    for (i = 0; i < 32768; i++) b8[i] = (Uint8)(b8[i] + 1); /* leer y luego escribir: linea ya en cache */
    t4 = nds_usec();
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(c1));
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(c2));
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(c3));
    __asm__ volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(r[0]));
    __asm__ volatile("mrc p15, 0, %0, c6, c1, 0" : "=r"(r[1]));
    __asm__ volatile("mrc p15, 0, %0, c6, c2, 0" : "=r"(r[2]));
    __asm__ volatile("mrc p15, 0, %0, c6, c3, 0" : "=r"(r[3]));
    __asm__ volatile("mrc p15, 0, %0, c6, c4, 0" : "=r"(r[4]));
    __asm__ volatile("mrc p15, 0, %0, c6, c5, 0" : "=r"(r[5]));
    __asm__ volatile("mrc p15, 0, %0, c6, c6, 0" : "=r"(r[6]));
    __asm__ volatile("mrc p15, 0, %0, c6, c7, 0" : "=r"(r[7]));
    snprintf(prof_txt[3], 40, "b%u w%u r%u rw%u %u", t1 - t0, t2 - t1, t3 - t2, t4 - t3, sum & 7);
    snprintf(prof_txt[4], 40, "c1 %lx c2 %lx c3 %lx", (unsigned long)c1, (unsigned long)c2, (unsigned long)c3);
    snprintf(prof_txt[5], 40, "%03lx/%lx %03lx/%lx %03lx/%lx %03lx/%lx",
        (unsigned long)(r[0] >> 20), (unsigned long)(r[0] & 63), (unsigned long)(r[1] >> 20), (unsigned long)(r[1] & 63),
        (unsigned long)(r[2] >> 20), (unsigned long)(r[2] & 63), (unsigned long)(r[3] >> 20), (unsigned long)(r[3] & 63));
    snprintf(prof_txt[6], 40, "%03lx/%lx %03lx/%lx %03lx/%lx %03lx/%lx",
        (unsigned long)(r[4] >> 20), (unsigned long)(r[4] & 63), (unsigned long)(r[5] >> 20), (unsigned long)(r[5] & 63),
        (unsigned long)(r[6] >> 20), (unsigned long)(r[6] & 63), (unsigned long)(r[7] >> 20), (unsigned long)(r[7] & 63));
}

unsigned nds_usec(void)
{
    u16 a, b, c;
    do {
        a = TIMER_DATA(1);
        b = TIMER_DATA(0);
        c = TIMER_DATA(1);
    } while (a != c);
    return (unsigned)a * 1000u + (unsigned)(u16)(b - (u16)TIMER_FREQ_64(1000)) * 1000u / 523u;
}

void nds_prof_add(int which, unsigned us)
{
    unsigned now;
    if (which < 0 || which > 10 || us > 1000000u)
        return;
    prof_sum[which] += us;
    prof_cnt[which]++;
    if (which != 4)
        return;
    now = nds_usec();
    if ((unsigned)(now - prof_last) < 1000000u && prof_last != 0)
        return;
    if ((unsigned)(now - prof_last) < 3000000u && prof_last != 0) {
#define PMS(i) (prof_cnt[i] ? prof_sum[i] / prof_cnt[i] / 100u : 0u)
        unsigned fr = prof_cnt[4];
        static int diag_done; /* NDS-RAPIDO2 */
        if (!diag_done) {
            diag_done = 1;
            nds_diag();
        }
        snprintf(prof_txt[0], sizeof prof_txt[0], "FPS %u  logica %u/s %u.%ums", fr, prof_cnt[0], PMS(0) / 10, PMS(0) % 10);
        snprintf(prof_txt[1], sizeof prof_txt[1], "mundo %u vista %u mapa %u", PMS(1) / 10, PMS(2) / 10, PMS(3) / 10); /* NDS-RAPIDO1 */
        { /* NDS-MEM: memoria del monton usada ahora / maximo visto (MB) */
            static unsigned mem_peak;
            const unsigned mem_used = (unsigned)(mallinfo().uordblks / 1024u);
            if (mem_used > mem_peak)
                mem_peak = mem_used;
            snprintf(prof_txt[2], sizeof prof_txt[2], "dibujo %u flip %u m%u.%u/%u.%uM", PMS(4) / 10, PMS(5) / 10,
                mem_used / 1024u, (mem_used % 1024u) * 10u / 1024u, mem_peak / 1024u, (mem_peak % 1024u) * 10u / 1024u);
        }
        snprintf(prof_txt[3], sizeof prof_txt[3], "j%u m%u o%u l%u v%u n%u/%u", PMS(6), PMS(7), PMS(8), PMS(9), PMS(10),
            prof_mon_run, prof_mon_total); /* NDS-RAPIDO5 + RAPIDO6 */
        {
            extern int nds_wifi_ok; /* NDS-MP1 */
            size_t L = strlen(prof_txt[3]);
            snprintf(prof_txt[3] + L, sizeof prof_txt[3] - L, " w%d", nds_wifi_ok);
        }
#undef PMS
    }
    memset(prof_sum, 0, sizeof prof_sum);
    memset(prof_cnt, 0, sizeof prof_cnt);
    prof_last = now;
}

const char *nds_prof_line(int n)
{
    return (n >= 0 && n < 7) ? prof_txt[n] : ""; /* NDS-RAPIDO2 */
}

/* NDS-ARRIBA: imagen propia en la pantalla de arriba durante los menus */
static int nds_top_img_on, nds_top_img_tried;
/* NDS-V12: texto de version sobre la imagen de arriba (solo menu principal) */
#define NDS_VER_TXT "V1.5"
#define VER_W (4 * 8 + 2)
#define VER_H 10
#define VER_X (LCD_W - VER_W - 3)
#define VER_Y (LCD_H - VER_H - 3)
static int ui_mainmenu, ver_drawn;
static u16 ver_save[VER_W * VER_H];
void nds_ui_mainmenu(int on) { ui_mainmenu = on; }
static void ver_update(void)
{
    u16 *vr = (u16 *)BG_BMP_RAM_SUB(0);
    const ConsoleFont *f = &consoleGetDefault()->font;
    int x, y, i;
    if (!nds_top_img_on) {
        ver_drawn = 0;
        return;
    }
    if (ui_mainmenu && !ver_drawn) {
        for (y = 0; y < VER_H; y++)
            for (x = 0; x < VER_W; x++)
                ver_save[y * VER_W + x] = vr[(VER_Y + y) * 256 + VER_X + x];
        for (i = 0; NDS_VER_TXT[i]; i++) {
            const int idx = (int)(unsigned char)NDS_VER_TXT[i] - (int)f->asciiOffset;
            if (idx < 0 || idx >= (int)f->numChars)
                continue;
            for (y = 0; y < 8; y++)
                for (x = 0; x < 8; x++) {
                    int on;
                    if (f->bpp == 1)
                        on = (((const u8 *)f->gfx)[idx * 8 + y] >> x) & 1;
                    else if (f->bpp == 4) {
                        const u8 bb = ((const u8 *)f->gfx)[idx * 32 + y * 4 + (x >> 1)];
                        on = (x & 1) ? (bb >> 4) : (bb & 15);
                    } else
                        on = ((const u8 *)f->gfx)[idx * 64 + y * 8 + x];
                    if (on) {
                        const int px = VER_X + 1 + i * 8 + x, py = VER_Y + 1 + y;
                        vr[(py + 1) * 256 + px + 1] = 0x8000; /* sombra negra */
                        vr[py * 256 + px] = 0x8000 | (12 << 10) | (26 << 5) | 31; /* dorado */
                    }
                }
        }
        ver_drawn = 1;
    } else if (!ui_mainmenu && ver_drawn) {
        for (y = 0; y < VER_H; y++)
            for (x = 0; x < VER_W; x++)
                vr[(VER_Y + y) * 256 + VER_X + x] = ver_save[y * VER_W + x];
        ver_drawn = 0;
    }
}
static void nds_top_img_show(void)
{
    if (nds_top_img_tried)
        return;
    nds_top_img_tried = 1;
    FILE *f = fopen(nds_r("/devilutionx/assets/ui_art/hellspawn_arriba.bin"), "rb");
    if (!f)
        return;
    u16 *buf = (u16 *)malloc(LCD_W * LCD_H * 2);
    if (buf && fread(buf, 2, LCD_W * LCD_H, f) == LCD_W * LCD_H) {
        REG_BG3CNT_SUB = BG_BMP16_256x256 | BG_MAP_BASE(0);
        DC_FlushRange(buf, LCD_W * LCD_H * 2);
        dmaCopyWords(3, buf, BG_BMP_RAM_SUB(0), LCD_W * LCD_H * 2);
        nds_top_img_on = 1;
        ver_drawn = 0; /* NDS-V12: imagen limpia */
    }
    free(buf);
    fclose(f);
}
static void nds_top_img_hide(void)
{
    REG_BG3CNT_SUB = BG_BMP8_256x256 | BG_MAP_BASE(4);
    dmaFillWords(0, video_vram_sub, LCD_W * LCD_H);
    nds_top_img_on = 0;
    nds_top_img_tried = 0; /* la proxima vez que se entre en los menus se vuelve a mostrar */
}

/* NDS-BESTIARIO6: lista del bestiario en la pantalla de arriba (8 bits -> 16 bits con la paleta) */
static int top_list_on;
void nds_top_list(const unsigned char *pix, int pitch)
{
    u16 *vr = (u16 *)BG_BMP_RAM_SUB(0);
    int x, y;
    if (!video_vram_sub)
        return;
    if (!pix) {
        if (top_list_on) {
            top_list_on = 0;
            nds_top_img_hide(); /* en el siguiente fotograma de menu vuelve la imagen de arriba */
            nds_apply_swap(); /* NDS-BESTIARIO6C: vuelve la eleccion de pantallas del jugador */
        }
        return;
    }
    if (!top_list_on) {
        REG_BG3CNT_SUB = BG_BMP16_256x256 | BG_MAP_BASE(0);
        top_list_on = 1;
        lcdMainOnBottom(); /* NDS-BESTIARIO6C: el monstruo siempre en la tactil, la lista siempre arriba */
    }
    nds_top_img_on = 1; /* que el fotograma de menu no la tape */
    nds_top_img_tried = 1;
    ver_drawn = 0;
    for (y = 0; y < LCD_H; y++) {
        const unsigned char *src = pix + y * pitch;
        u16 *d = vr + y * 256;
        for (x = 0; x < LCD_W; x++)
            d[x] = (u16)(0x8000 | BG_PALETTE[src[x]]);
    }
}

/* NDS-AMISTAD: boton PAZ/PVP en la esquina de la tactil (solo multijugador) */
static int ui_friendly; /* 0 = sin multijugador, 1 = amistoso, 2 = agresivo */
static volatile int nds_friendly_req;
void nds_ui_friendly(int f) { ui_friendly = f; }
int nds_take_friendly(void)
{
    const int r = nds_friendly_req;
    nds_friendly_req = 0;
    return r;
}
#define FB_X (LCD_W - 32)
#define FB_Y 2
#define FB_W 30
#define FB_H 13
/* NDS-NIVEL: boton "+" arriba a la izquierda de la tactil (puntos sin repartir) */
#define LB_X 2
#define LB_Y 2
#define LB_W 14
#define LB_H 13
static void lb_draw(Uint8 *dst)
{
    static Uint8 rows[LCD_W * (LB_Y + LB_H)];
    int x, y;
    Uint8 cBg, cCol;
    if (!ui_statpts || !dst)
        return;
    cBg = (Uint8)vk_near(2, 2, 2);
    cCol = (Uint8)vk_near(31, 26, 12);
    for (y = 0; y < LB_Y + LB_H; y++) { /* leer de la VRAM de 2 en 2 bytes */
        const u16 *sp = (const u16 *)(dst + y * LCD_W);
        u16 *dp = (u16 *)(rows + y * LCD_W);
        for (x = 0; x < LCD_W / 2; x++)
            dp[x] = sp[x];
    }
    for (y = 0; y < LB_H; y++)
        for (x = 0; x < LB_W; x++)
            rows[(LB_Y + y) * LCD_W + LB_X + x] = (y == 0 || y == LB_H - 1 || x == 0 || x == LB_W - 1) ? cCol : cBg;
    fnt_char(rows, LB_X + 3, LB_Y + 3, (unsigned char)'+', cCol);
    for (y = LB_Y; y < LB_Y + LB_H; y++) { /* escribir en la VRAM de 2 en 2 bytes */
        const u16 *sp = (const u16 *)(rows + y * LCD_W);
        u16 *dp = (u16 *)(dst + y * LCD_W);
        for (x = 0; x < LCD_W / 2; x++)
            dp[x] = sp[x];
    }
}
static void fb_draw(Uint8 *dst)
{
    static Uint8 rows[LCD_W * (FB_Y + FB_H)];
    int x, y, i;
    const char *t;
    Uint8 cBg, cCol;
    if (!ui_friendly || !dst)
        return;
    cBg = (Uint8)vk_near(2, 2, 2);
    cCol = ui_friendly == 1 ? (Uint8)vk_near(8, 26, 10) : (Uint8)vk_near(30, 6, 6);
    for (y = 0; y < FB_Y + FB_H; y++) { /* leer de la VRAM de 2 en 2 bytes */
        const u16 *sp = (const u16 *)(dst + y * LCD_W);
        u16 *dp = (u16 *)(rows + y * LCD_W);
        for (x = 0; x < LCD_W / 2; x++)
            dp[x] = sp[x];
    }
    for (y = 0; y < FB_H; y++)
        for (x = 0; x < FB_W; x++)
            rows[(FB_Y + y) * LCD_W + FB_X + x] = (y == 0 || y == FB_H - 1 || x == 0 || x == FB_W - 1) ? cCol : cBg;
    t = ui_friendly == 1 ? "PAZ" : "PVP";
    for (i = 0; i < 3; i++)
        fnt_char(rows, FB_X + 3 + i * 8, FB_Y + 3, (unsigned char)t[i], cCol);
    for (y = FB_Y; y < FB_Y + FB_H; y++) { /* escribir en la VRAM de 2 en 2 bytes */
        const u16 *sp = (const u16 *)(rows + y * LCD_W);
        u16 *dp = (u16 *)(dst + y * LCD_W);
        for (x = 0; x < LCD_W / 2; x++)
            dp[x] = sp[x];
    }
}

int SDL_Flip(SDL_Surface *s)
{
    hb_flip++;
    hb_last = "Flip";
    guard_check();
    nds_lua_probe("Flip");
    vwall_check(8, (unsigned)__builtin_return_address(0));
    nds_lua_light(8, (unsigned)__builtin_return_address(0));
    (void)s;
    if (!video_ready)
        return -1;
    unsigned prof_f0 = nds_usec(); /* NDS-PROF */
    { /* NDS-RAPIDO4: doble pagina en la VRAM (pagina 0 = base 0, pagina 1 = +64 KB) */
        static int vpage_back = 1;
        Uint8 *back = (Uint8 *)video_vram + (vpage_back ? 0x10000 : 0);
        while (vpage_show >= 0) /* la pagina anterior aun no se ha mostrado */
            ;
        if (ui_mode == 0) {
            vback = back;
            nds_ui_compose(); /* el mundo va directo a la VRAM, 4 pixeles por escritura */
            if (!nds_get_swap()) { /* NDS-HUDFIX: con "Juego arriba" van en la tactil (HUD) */
                fb_draw(back); /* NDS-AMISTAD */
                lb_draw(back); /* NDS-NIVEL */
            }
            vback = NULL;
        } else {
            DC_FlushRange(video_pixels, SCREEN_BYTES);
            nds_ui_compose(); /* NDS-ZOOM: siempre se compone (el mundo se reduce) */
            DC_FlushRange(comp_buf, LCD_W * LCD_H);
            dmaCopyWords(3, comp_buf, back, LCD_W * LCD_H);
        }
        vpage_show = vpage_back;
        vpage_back ^= 1;
    }
    while (dmaBusy(2)) /* NDS-RAPIDO5: no pisar la copia del HUD */
        ;
    if (nds_top_img_on && ui_mode < 4) /* NDS-ARRIBA: se sale de los menus: arriba vuelve a 8 bits */
        nds_top_img_hide();
    if (video_vram_sub && ui_mode >= 4) { /* menus y videos: imagen propia arriba (NDS-ARRIBA) o negro */
        if (!nds_top_img_on)
            nds_top_img_show();
        if (!nds_top_img_on)
            dmaFillWords(0, video_vram_sub, LCD_W * LCD_H);
        ver_update(); /* NDS-V12 */
    }
    else if (video_vram_sub && hud_pan) { /* NDS-HUD */
        static unsigned hud_n; /* NDS-RAPIDO4: HUD de arriba 1 de cada 2 fotogramas (NDS-RAPIDO7) */
        if (hud_n++ & 1)
            goto hud_skip;
        nds_hud_compose();
        if (ui_mode == 0 && nds_get_swap()) { /* NDS-HUDFIX: botones en la tactil */
            fb_draw(top_buf);
            lb_draw(top_buf);
        }
        DC_FlushRange(top_buf, sizeof top_buf);
        dmaCopyWordsAsynch(2, top_buf, video_vram_sub, sizeof top_buf); /* NDS-RAPIDO5: en segundo plano */
    hud_skip:;
    }
    else if (video_vram_sub)
        dmaFillWords(0, video_vram_sub, LCD_W * LCD_H);
    nds_prof_add(5, nds_usec() - prof_f0);
    nds_mem_tick(); /* NDS-MEMTRK */
    nds_audio_update();
    if (nds_net_hook) /* NDS-MP4: atender la red local cada fotograma */
        nds_net_hook();
    thrd_yield();
    return 0;
}

void SDL_VideoQuit(void) { video_ready = 0; }

/* ---------------- inicio / cierre ---------------- */
int SDL_Init(Uint32 flags)
{
    (void)flags;
    if (!timer_ready)
        timer_init();
    return 0;
}
Uint32 SDL_WasInit(Uint32 flags) { return video_ready ? (flags ? flags : SDL_INIT_VIDEO) : 0; }
void SDL_Quit(void) { SDL_VideoQuit(); }

int SDL_EnableUNICODE(int enable) { (void)enable; return 0; }
int SDL_ShowCursor(int toggle) { (void)toggle; return 0; }
void SDL_WarpMouse(Uint16 x, Uint16 y) { (void)x; (void)y; }
SDL_GrabMode SDL_WM_GrabInput(SDL_GrabMode mode) { (void)mode; return SDL_GRAB_OFF; }
void SDL_WM_SetCaption(const char *title, const char *icon) { (void)title; (void)icon; }
int SDL_NumJoysticks(void) { return 0; }
SDL_Joystick *SDL_JoystickOpen(int index) { (void)index; return NULL; }
const char *SDL_JoystickName(int index) { (void)index; return NULL; }

/* ---------------- etapa 3: archivos ---------------- */
static int rw_seek(SDL_RWops *c, int offset, int whence)
{
    FILE *fp = (FILE *)c->hidden.unknown.data1;
    if (fseek(fp, offset, whence) != 0)
        return -1;
    return (int)ftell(fp);
}

static int rw_read(SDL_RWops *c, void *ptr, int size, int maxnum)
{
    FILE *fp = (FILE *)c->hidden.unknown.data1;
    return (int)fread(ptr, (size_t)size, (size_t)maxnum, fp);
}

static int rw_write(SDL_RWops *c, const void *ptr, int size, int num)
{
    FILE *fp = (FILE *)c->hidden.unknown.data1;
    return (int)fwrite(ptr, (size_t)size, (size_t)num, fp);
}

static int rw_close(SDL_RWops *c)
{
    int r = 0;
    if (c) {
        FILE *fp = (FILE *)c->hidden.unknown.data1;
        if (fp)
            r = (fclose(fp) == 0) ? 0 : -1;
        free(c);
    }
    return r;
}

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode)
{
    FILE *fp;
    SDL_RWops *c;
    if (!file || !mode) {
        SDL_SetError("RWFromFile: argumentos invalidos");
        return NULL;
    }
    fp = fopen(file, mode);
    if (!fp) {
        SDL_SetError("RWFromFile: no se pudo abrir %s", file);
        return NULL;
    }
    c = (SDL_RWops *)calloc(1, sizeof(SDL_RWops));
    if (!c) {
        fclose(fp);
        SDL_SetError("sin memoria");
        return NULL;
    }
    c->seek = rw_seek;
    c->read = rw_read;
    c->write = rw_write;
    c->close = rw_close;
    c->hidden.unknown.data1 = fp;
    return c;
}

/* ---------------- etapa 4: hilos cooperativos (cothread de BlocksDS) ---------------- */
#define NDS_THREAD_STACK (128 * 1024)

typedef struct {
    thrd_t id;
} NdsThread;

SDL_Thread *SDL_CreateThread(int (*fn)(void *), void *data)
{
    NdsThread *t = (NdsThread *)calloc(1, sizeof(NdsThread));
    if (!t) {
        SDL_SetError("sin memoria para el hilo");
        return NULL;
    }
    t->id = cothread_create((cothread_entrypoint_t)fn, data, NDS_THREAD_STACK, 0);
    hb_loader = t->id;
    hb_last = "Create";
    if (t->id < 0) {
        free(t);
        SDL_SetError("cothread_create ha fallado");
        return NULL;
    }
    return (SDL_Thread *)t;
}

Uint32 SDL_GetThreadID(SDL_Thread *th)
{
    if (th)
        return (Uint32)((NdsThread *)th)->id + 2;
    return (Uint32)thrd_current() + 2;
}

void SDL_WaitThread(SDL_Thread *th, int *status)
{
    NdsThread *t = (NdsThread *)th;
    int res = 0;
    if (!t)
        return;
    hb_last = "Wait";
    thrd_join(t->id, &res);
    if (status)
        *status = res;
    free(t);
}

typedef struct {
    mtx_t m;
    thrd_t owner;
    int count;
} NdsMutex;

SDL_mutex *SDL_CreateMutex(void)
{
    NdsMutex *n = (NdsMutex *)calloc(1, sizeof(NdsMutex));
    if (!n)
        return NULL;
    mtx_init(&n->m, mtx_plain);
    n->owner = -1;
    return (SDL_mutex *)n;
}

int SDL_mutexP(SDL_mutex *mm)
{
    hb_mtx++;
    hb_last = "MutexP";
    NdsMutex *n = (NdsMutex *)mm;
    thrd_t me;
    if (!n)
        return -1;
    me = thrd_current();
    if (n->count > 0 && n->owner == me) {
        n->count++;
        return 0;
    }
    mtx_lock(&n->m);
    n->owner = me;
    n->count = 1;
    return 0;
}

int SDL_mutexV(SDL_mutex *mm)
{
    NdsMutex *n = (NdsMutex *)mm;
    if (!n)
        return -1;
    if (n->count > 0 && --n->count == 0) {
        n->owner = -1;
        mtx_unlock(&n->m);
    }
    return 0;
}

void SDL_DestroyMutex(SDL_mutex *mm) { free(mm); }

/* ---------------- etapa 2: entrada (botones y tactil) ---------------- */
#define EVQ_SIZE 64
static SDL_Event evq[EVQ_SIZE];
static unsigned evq_head, evq_tail;
static Uint8 key_state[512];
static u32 prev_held;
static SDLKey pressed_sym[32]; /* NDS-BOTONES2 */
static int ui_menu_keys; /* NDS-BOTONES3: 1 = menu dentro de la partida */
void nds_ui_menu_keys(int on) { ui_menu_keys = on; }
static int touch_down, mouse_x, mouse_y;
static Uint8 mouse_buttons;

static const struct { u32 ds; SDLKey sym; } keymap[] = {
    { KEY_UP, SDLK_UP },     { KEY_DOWN, SDLK_DOWN },     { KEY_LEFT, SDLK_LEFT },  { KEY_RIGHT, SDLK_RIGHT },
    { KEY_A, SDLK_LALT },    { KEY_B, SDLK_LCTRL },       { KEY_X, SDLK_LSHIFT },   { KEY_Y, SDLK_SPACE },
    { KEY_L, SDLK_TAB },     { KEY_R, SDLK_BACKSPACE },   { KEY_START, SDLK_RETURN }, { KEY_SELECT, SDLK_ESCAPE },
};

static void evq_push(const SDL_Event *e)
{
    unsigned next = (evq_tail + 1) % EVQ_SIZE;
    if (next == evq_head)
        return; /* cola llena: se pierde el evento mas nuevo */
    evq[evq_tail] = *e;
    evq_tail = next;
}

static void push_motion(int x, int y, int xrel, int yrel)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.motion.type = SDL_MOUSEMOTION;
    e.motion.state = mouse_buttons;
    e.motion.x = (Uint16)x;
    e.motion.y = (Uint16)y;
    e.motion.xrel = (Sint16)xrel;
    e.motion.yrel = (Sint16)yrel;
    evq_push(&e);
}

static void push_button(int down, int x, int y)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.x = (Uint16)x;
    e.button.y = (Uint16)y;
    evq_push(&e);
}

/* NDS-CINTURON: doble toque en el inventario o el cinturon = Shift+clic (mover solo) */
static SDLMod nds_mod;
static Uint32 dt_last;
static int dt_x, dt_y, dt_eat;
static void push_button_shift(int down, int x, int y)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.button.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.which = 1; /* marca: se entrega con Shift */
    e.button.button = SDL_BUTTON_LEFT;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.x = (Uint16)x;
    e.button.y = (Uint16)y;
    evq_push(&e);
}
static int dt_check(int x, int y)
{
    const Uint32 now = SDL_GetTicks();
    if (!belt_on() || !(x >= LCD_W - UI_PW || belt_hit(x, y, 0, 0, 0))) {
        dt_last = 0;
        return 0;
    }
    if (dt_last && now - dt_last < 400u && abs(x - dt_x) < 12 && abs(y - dt_y) < 12) {
        dt_last = 0;
        return 1;
    }
    dt_last = now | 1u;
    dt_x = x;
    dt_y = y;
    return 0;
}
static int fb_touch; /* NDS-AMISTAD: el lapiz esta sobre el boton PAZ/PVP */
static int fb_check(const touchPosition *t)
{
    if (fb_touch)
        return 1;
    if (!touch_down && ui_statpts && ui_mode == 0 && t->px < LB_X + LB_W + 2 && t->py < LB_Y + LB_H + 2) { /* NDS-NIVEL */
        fb_touch = 1;
        nds_chr_req = 1;
        return 1;
    }
    if (!touch_down && ui_friendly && ui_mode == 0 && t->px >= FB_X && t->py < FB_Y + FB_H + 2) {
        fb_touch = 1;
        nds_friendly_req = 1;
        return 1;
    }
    return 0;
}
/* NDS-ATAJO */
static volatile int nds_warp_req;
int nds_take_warp(void)
{
    const int r = nds_warp_req;
    nds_warp_req = 0;
    return r;
}

static void input_poll(void)
{
    u32 held, changed;
    unsigned i;
    touchPosition t;

    scanKeys();
    held = keysHeld();
    changed = held ^ prev_held;
    if (!(held & KEY_TOUCH)) /* NDS-AMISTAD */
        fb_touch = 0;
    { /* NDS-ATAJO: L+R mantenidos 2 s piden bajar un nivel */
        static Uint32 lr_since;
        static int lr_done;
        if ((held & (KEY_L | KEY_R)) == (KEY_L | KEY_R)) {
            if (!lr_since)
                lr_since = SDL_GetTicks() | 1u;
            else if (!lr_done && SDL_GetTicks() - lr_since >= 2000u) {
                nds_warp_req = 1;
                lr_done = 1;
            }
        } else {
            lr_since = 0;
            lr_done = 0;
        }
    }
    for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        if (changed & keymap[i].ds) {
            int down = (held & keymap[i].ds) != 0;
            SDL_Event e;
            memset(&e, 0, sizeof(e));
            e.key.type = down ? SDL_KEYDOWN : SDL_KEYUP;
            e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
            {
                SDLKey sym = keymap[i].sym;
                if (down) {
                    /* en partida (HUD activo y sin menus de inicio): A ataca (Ctrl) y B es Alt */
                    if (hud_pan && ui_mode < 4 && !ui_menu_keys) {
                        if (sym == SDLK_LCTRL)
                            sym = SDLK_LALT;
                        else if (sym == SDLK_LALT)
                            sym = SDLK_LCTRL;
                    }
                    pressed_sym[i] = sym;
                } else if (pressed_sym[i]) {
                    sym = pressed_sym[i]; /* soltar la misma tecla que se pulso */
                }
                e.key.keysym.sym = sym;
                key_state[sym] = (Uint8)down;
            }
            e.key.keysym.mod = KMOD_NONE;
            evq_push(&e);
        }
    }
    prev_held = held;

    if (held & KEY_TOUCH) {
        touchRead(&t);
        if ((t.px != 0 || t.py != 0) && !fb_check(&t)) { /* (0,0) = lectura invalida; NDS-AMISTAD: el boton no es un clic */
            const int rx = t.px, ry = t.py; /* NDS-CINTURON */
            nds_touch_map(&t.px, &t.py);
            if (!touch_down) {
                touch_down = 1;
                mouse_x = t.px;
                mouse_y = t.py;
                push_motion(mouse_x, mouse_y, 0, 0);
                mouse_buttons = SDL_BUTTON(1);
                if (dt_check(rx, ry)) { /* NDS-CINTURON: soltar lo cogido con el 1er toque y moverlo con Shift */
                    push_button(1, mouse_x, mouse_y);
                    push_button(0, mouse_x, mouse_y);
                    push_button_shift(1, mouse_x, mouse_y);
                    push_button_shift(0, mouse_x, mouse_y);
                    dt_eat = 1;
                } else
                    push_button(1, mouse_x, mouse_y);
            } else if (!dt_eat && (t.px != mouse_x || t.py != mouse_y)) {
                push_motion(t.px, t.py, t.px - mouse_x, t.py - mouse_y);
                mouse_x = t.px;
                mouse_y = t.py;
            }
        }
    } else if (touch_down) {
        touch_down = 0;
        mouse_buttons = 0;
        if (dt_eat) /* NDS-CINTURON: el soltar ya se mando */
            dt_eat = 0;
        else
            push_button(0, mouse_x, mouse_y);
    }
}

int SDL_PollEvent(SDL_Event *e)
{
    nds_lua_light(4, (unsigned)__builtin_return_address(0));
    hb_poll++;
    hb_last = "Poll";
    if (evq_head == evq_tail)
        input_poll();
    if (evq_head == evq_tail)
        return 0;
    if (e) {
        *e = evq[evq_head];
        evq_head = (evq_head + 1) % EVQ_SIZE;
        if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP) /* NDS-CINTURON */
            nds_mod = e->button.which == 1 ? KMOD_LSHIFT : KMOD_NONE;
    }
    return 1;
}
int SDL_PushEvent(SDL_Event *e) { evq_push(e); return 0; }
Uint8 *SDL_GetKeyState(int *numkeys)
{
    input_poll();
    if (numkeys)
        *numkeys = 512;
    return key_state;
}
Uint8 SDL_GetMouseState(int *x, int *y)
{
    input_poll();
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    return mouse_buttons;
}
SDLMod SDL_GetModState(void) { return nds_mod; } /* NDS-CINTURON */

/* ---------------- etapa 3: SD y rutas ---------------- */
#include <fat.h>
#include <dswifi9.h> /* NDS-MP1 */
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static char arg0[] = "devilutionx";
static char arg1[] = "--data-dir";
static char arg2[48] = "sd:" "/devilutionx/";
static char arg3[] = "--save-dir";
static char arg4[48] = "sd:" "/devilutionx/saves/";
static char arg5[] = "--config-dir";
static char arg6[48] = "sd:" "/devilutionx/saves/";
static char arg7[] = "--verbose";
static char *nds_args[] = { arg0, arg1, arg2, arg3, arg4, arg5, arg6, NULL };
int nds_fat_ok;


/* NDS-ERROR: comprobacion de la SD al arrancar; si falta algo se avisa y se detiene el juego */
static int pf_has_dir(const char *path)
{
    DIR *d = opendir(path);
    if (d)
        closedir(d);
    return d != NULL;
}

static int pf_has_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

static int pf_can_write(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    const int ok = fwrite("ok", 1, 2, f) == 2;
    fclose(f);
    remove(path);
    return ok;
}

static void pf_line(int ok, const char *what)
{
    printf("%s %s\n", ok ? " ok  " : "FALTA", what);
}

static void nds_stop_screen(void)
{
    printf("\n Copia lo que falta en la SD\n");
    printf(" y vuelve a abrir el juego.\n\n");
    printf(" START: volver al menu\n");
    for (;;) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START)
            exit(0);
    }
}

static void nds_preflight(void)
{
    const int assets = pf_has_dir(nds_r("/devilutionx/assets"));
    const int pal = pf_has_file(nds_r("/devilutionx/assets/ui_art/diablo.pal"));
    const int full = pf_has_dir(nds_r("/devilutionx/diabdat"));
    const int demo = pf_has_dir(nds_r("/devilutionx/spawn"));
    const char *data = full ? nds_r("/devilutionx/diabdat") : nds_r("/devilutionx/spawn");
    char p1[64], p2[64];
    snprintf(p1, sizeof p1, "%s/levels", data);
    snprintf(p2, sizeof p2, "%s/ui_art", data);
    const int lv = (full || demo) && pf_has_dir(p1);
    const int ui = (full || demo) && pf_has_dir(p2);
    const int wr = pf_can_write(nds_r("/devilutionx/saves/prueba_sd.tmp"));
    const int ok = assets && pal && (full || demo) && lv && ui && wr;
    printf("datos: %s\n", full ? "version completa" : (demo ? "demo (spawn)" : "ninguno"));
    if (ok)
        return;
    consoleClear();
    printf("===============================\n");
    printf("   FALTAN ARCHIVOS EN LA SD\n");
    printf("===============================\n");
    pf_line(assets, "devilutionx/assets/");
    pf_line(pal, "assets/ui_art/diablo.pal");
    if (full || demo) {
        pf_line(1, full ? "devilutionx/diabdat/" : "devilutionx/spawn/");
        pf_line(lv, full ? "diabdat/levels/" : "spawn/levels/");
        pf_line(ui, full ? "diabdat/ui_art/" : "spawn/ui_art/");
    } else {
        pf_line(0, "devilutionx/diabdat/");
        printf("      (o spawn/ para la demo)\n");
    }
    pf_line(wr, "escribir en saves/");
    if (!assets || !pal)
        printf("\n assets/ sale de la carpeta\n build-nds/assets del PC.\n");
    if (!(full || demo) || !lv || !ui)
        printf("\n diabdat/ es diabdat.mpq\n desempaquetado (carpetas\n levels, ui_art, ...).\n");
    if (!wr)
        printf("\n La SD no deja escribir:\n mira si esta bloqueada.\n");
    nds_stop_screen();
}

#define PAGE_CHARS 600
static int (*orig_put_out)(char, FILE *);
static int (*orig_put_err)(char, FILE *);
static size_t page_count;

static void page_check(FILE *f, int (*orig)(char, FILE *))
{
    if (no_page)
        return;
    if (++page_count >= PAGE_CHARS) {
        static const char msg[] = "\n[pagina: pulsa A]\n";
        const char *q;
        for (q = msg; *q; q++)
            orig(*q, f);
        do {
            swiWaitForVBlank();
            scanKeys();
        } while (!(keysDown() & KEY_A));
        consoleClear();
        page_count = 0;
    }
}

static int paged_put_out(char c, FILE *f)
{
    int r = orig_put_out(c, f);
    page_check(f, orig_put_out);
    return r;
}

static int paged_put_err(char c, FILE *f)
{
    int r = orig_put_err(c, f);
    page_check(f, orig_put_err);
    return r;
}

static void nds_limit_log(void)
{
    orig_put_out = stdout->put;
    stdout->put = paged_put_out;
    if (stderr != stdout) {
        orig_put_err = stderr->put;
        stderr->put = paged_put_err;
    }
}

/* NDS-MP1: wifi en modo local (multijugador DS a DS), sin conectarse a nada */
int nds_wifi_ok = -1;

void nds_platform_init(int *argc, char ***argv)
{
    static int done;
    if (!done) {
        done = 1;
        consoleDemoInit();
    setExceptionHandler(nds_exception); /* consola de depuracion en la pantalla secundaria */
        if (isDSiMode())
            setCpuClock(true);
        printf("modo DSi: %s\n", isDSiMode() ? "SI" : "NO"); /* NDS-MODO */
        if (!isDSiMode()) {
            printf("\nEste juego necesita modo DSi\n(16 MB de RAM).\n\nEn TWiLight Menu++, ajustes del\njuego: Run in -> DSi mode,\no desactiva nds-bootstrap.\n");
            for (;;)
                swiWaitForVBlank();
        }
        hb_start();
        irqSet(IRQ_VBLANK, nds_vblank);
        irqEnable(IRQ_VBLANK);
        nds_fat_ok = fatInitDefault();
        { /* NDS-RAIZ: si solo existe fat:/devilutionx (melonDS + DLDI) se usa fat: */
            DIR *d2 = opendir("sd:" "/devilutionx"); /* NDS-RAIZ2: fat: solo si no hay sd: */
            DIR *d1 = d2 ? NULL : opendir("fat:/devilutionx");
            if (d1 && !d2) { strcpy(nds_root, "fat:"); nds_fat_ok = 1; }
            if (d1) closedir(d1);
            if (d2) closedir(d2);
            snprintf(arg2, sizeof arg2, "%s/devilutionx/", nds_root);
            snprintf(arg4, sizeof arg4, "%s/devilutionx/saves/", nds_root);
            snprintf(arg6, sizeof arg6, "%s/devilutionx/saves/", nds_root);
            printf("datos en: %s/devilutionx\n", nds_root);
        }
        /* NDS-MP20: la wifi se enciende al entrar en "DSi local" (protocol_nds.cpp), no al arrancar */
        if (nds_fat_ok) {
            mkdir(nds_r("/devilutionx"), 0777);
            mkdir(nds_r("/devilutionx/saves"), 0777);
            chdir(nds_r("/devilutionx/"));
            {
                char cwd[96];
                printf("cwd: %s\n", getcwd(cwd, sizeof(cwd)) ? cwd : "?");
            }
            nds_preflight();
            if (NDS_DEBUG) {
                for (int i = 0; i < 300; i++)
                    swiWaitForVBlank();
                nds_limit_log();
            }
        } else {
            consoleClear(); /* NDS-ERROR */
            printf("===============================\n");
            printf("   NO SE PUEDE LEER LA SD\n");
            printf("===============================\n\n");
            printf(" Revisa que la tarjeta este\n bien puesta y abre el juego\n desde la SD.\n");
            nds_stop_screen();
        }
    }
    if (argc && argv) {
        *argc = 7;
        *argv = nds_args;
    }
}

int SDL_SetColors(SDL_Surface *s, SDL_Color *colors, int first, int n)
{
    return SDL_SetPalette(s, SDL_LOGPAL | SDL_PHYSPAL, colors, first, n);
}

/* ---------- audio (NDS-AUDIO): efectos con canales de hardware y musica por streaming ---------- */
#define MUS_RB 32768 /* muestras por canal en el anillo (1,5 s a 22050 Hz); potencia de 2 */
#define MUS_CHUNK 1024
static s16 *mus_buf[2];
static int mus_on, mus_ch[2] = { -1, -1 }, mus_stereo, mus_vol = 127;
static u32 mus_written, mus_played;
static u16 mus_t3_last; /* (ya no se usa: NDS-MP1) */
/* NDS-MP1: posicion de la musica sin temporizadores 2/3 (la wifi usa el 3).
   Los temporizadores 0/1 cuentan ticks del bus/64 (periodo de 523 ticks por "ms").
   El canal de sonido avanza una muestra cada mus_period ticks del bus. */
static u32 mus_period, mus_tick_last, mus_bus_acc;
#define MUS_T0P ((u32)(0x10000u - (u16)TIMER_FREQ_64(1000))) /* NDS-MUSRELOJ: tics reales por vuelta del temporizador 0 (524) */
static u32 mus_ticks64(void)
{
    u16 a, b, c;
    do {
        a = TIMER_DATA(1);
        b = TIMER_DATA(0);
        c = TIMER_DATA(1);
    } while (a != c);
    return (u32)a * MUS_T0P + (u32)(u16)(b - (u16)TIMER_FREQ_64(1000)); /* NDS-MUSRELOJ */
}
static u32 mus_advance(void)
{
    const u32 wrap = 65536u * MUS_T0P; /* NDS-MUSRELOJ */
    u32 now = mus_ticks64(), d, n;
    d = now >= mus_tick_last ? now - mus_tick_last : now + wrap - mus_tick_last;
    mus_tick_last = now;
    mus_bus_acc += d * 64u;
    n = mus_period ? mus_bus_acc / mus_period : 0;
    mus_bus_acc -= n * mus_period;
    return n;
}
static int (*mus_read)(void *buf, int bytes);
static volatile int aud_busy;
static s16 mus_tmp[MUS_CHUNK * 2];
static int snd_ready;
static s32 mus_target = MUS_RB - MUS_CHUNK - 512; /* NDS-VIDAUDIO: cuanto se adelanta el relleno */
volatile unsigned nds_aud_underruns;

int nds_snd_init(void)
{
    if (!snd_ready) {
        soundEnable();
        snd_ready = 1;
    }
    return 1;
}

int nds_snd_play(const void *pcm, int bytes, int is16, int freq, int vol, int pan)
{
    if (!snd_ready || !pcm || bytes < 4)
        return -1;
    DC_FlushRange(pcm, (u32)bytes);
    return soundPlaySample(pcm, is16 ? SoundFormat_16Bit : SoundFormat_8Bit, (u32)bytes, (u16)freq,
        (u8)vol, (u8)pan, false, 0);
}

void nds_snd_kill(int ch)
{
    if (ch >= 0 && ch < 16)
        soundKill(ch);
}

static void mus_fill(void)
{
    const int fbytes = mus_stereo ? 4 : 2;
    mus_played += mus_advance(); /* NDS-MP1 */
    if ((s32)(mus_played - mus_written) > 0) { /* se quedo sin datos (carga larga): saltar adelante */
        mus_written = (mus_played + MUS_CHUNK) & ~(u32)(MUS_CHUNK - 1);
        nds_aud_underruns++;
    }
    while ((s32)(mus_written - mus_played) <= mus_target) {
        u32 pos = mus_written & (MUS_RB - 1);
        int i, got = mus_read(mus_tmp, MUS_CHUNK * fbytes);
        if (got < MUS_CHUNK * fbytes)
            memset((u8 *)mus_tmp + (got > 0 ? got : 0), 0, (size_t)(MUS_CHUNK * fbytes - (got > 0 ? got : 0)));
        if (mus_stereo) {
            s16 *l = mus_buf[0] + pos, *r = mus_buf[1] + pos;
            for (i = 0; i < MUS_CHUNK; i++) {
                l[i] = mus_tmp[2 * i];
                r[i] = mus_tmp[2 * i + 1];
            }
            DC_FlushRange(l, MUS_CHUNK * 2);
            DC_FlushRange(r, MUS_CHUNK * 2);
        } else {
            memcpy(mus_buf[0] + pos, mus_tmp, MUS_CHUNK * 2);
            DC_FlushRange(mus_buf[0] + pos, MUS_CHUNK * 2);
        }
        mus_written += MUS_CHUNK;
    }
}

void nds_mus_stop(void)
{
    int i;
    while (aud_busy) /* NDS-MP1B: otro hilo esta rellenando la musica */
        thrd_yield();
    if (!mus_on)
        return;
    mus_on = 0;
    for (i = 0; i < 2; i++) {
        if (mus_ch[i] >= 0)
            soundKill(mus_ch[i]);
        mus_ch[i] = -1;
    }
    for (i = 0; i < 2; i++) { /* NDS-MP1: sin temporizadores 2/3 */
        free(mus_buf[i]);
        mus_buf[i] = NULL;
    }
}

static int nds_mus_start_ex(int freq, int stereo, int (*rd)(void *buf, int bytes), s32 target)
{
    int i, n = stereo ? 2 : 1;
    nds_mus_stop();
    mus_target = target;
    if (!snd_ready || freq <= 0 || !rd)
        return 0;
    for (i = 0; i < n; i++) {
        mus_buf[i] = (s16 *)memalign(32, MUS_RB * 2);
        if (!mus_buf[i]) {
            free(mus_buf[0]);
            mus_buf[0] = NULL;
            return 0;
        }
    }
    mus_read = rd;
    mus_stereo = stereo;
    mus_written = 0;
    mus_played = 0;
    mus_t3_last = 0;
    mus_period = 2u * (0x1000000u / (u32)freq); /* NDS-MP1 */
    mus_bus_acc = 0;
    mus_tick_last = mus_ticks64();
    aud_busy = 1;
    mus_fill(); /* llenar el anillo antes de empezar */
    aud_busy = 0;
    for (i = 0; i < n; i++)
        mus_ch[i] = soundPlaySample(mus_buf[i], SoundFormat_16Bit, MUS_RB * 2, (u16)freq, (u8)mus_vol,
            stereo ? (i == 0 ? 0 : 127) : 64, true, 0);
    mus_tick_last = mus_ticks64(); /* NDS-MP1: la cuenta empieza al arrancar los canales */
    mus_bus_acc = 0;
    mus_on = 1;
    return mus_ch[0] >= 0;
}

void nds_mus_volume(int vol)
{
    int i;
    mus_vol = vol < 0 ? 0 : (vol > 127 ? 127 : vol);
    for (i = 0; i < 2; i++)
        if (mus_on && mus_ch[i] >= 0)
            soundSetVolume(mus_ch[i], (u8)mus_vol);
}

void nds_audio_update(void)
{
    if (!mus_on || aud_busy)
        return;
    aud_busy = 1;
    mus_fill();
    aud_busy = 0;
}

int nds_mus_start(int freq, int stereo, int (*rd)(void *buf, int bytes))
{
    return nds_mus_start_ex(freq, stereo, rd, MUS_RB - MUS_CHUNK - 512);
}

/* ---------- audio de las cinematicas (NDS-VIDAUDIO) ---------- */
#define VID_FIFO 65536 /* muestras de 16 bits (intercaladas si es estereo) */
static s16 *vid_fifo;
static u32 vid_w, vid_r;
static int vid_bits;

static int vid_read(void *buf, int bytes)
{
    s16 *o = (s16 *)buf;
    int i, n = bytes / 2;
    for (i = 0; i < n; i++)
        o[i] = (vid_r != vid_w) ? vid_fifo[vid_r++ & (VID_FIFO - 1)] : 0;
    return bytes;
}

void nds_vid_audio_begin(int rate, int channels, int bits, int vol)
{
    free(vid_fifo);
    vid_fifo = (s16 *)malloc(VID_FIFO * 2);
    vid_w = vid_r = 0;
    vid_bits = bits;
    if (!vid_fifo)
        return;
    nds_mus_volume(vol);
    /* poco adelanto (unos 0,2 s) para que el sonido no vaya retrasado respecto a la imagen */
    nds_mus_start_ex(rate, channels == 2, vid_read, 13230); /* NDS-DSVMARGEN: unos 0,6 s de margen en el anillo (antes 4096 = 0,19 s) */
}

void nds_vid_audio_push(const void *data, int bytes)
{
    int i, n;
    if (!vid_fifo)
        return;
    n = vid_bits == 16 ? bytes / 2 : bytes;
    if ((s32)(vid_w - vid_r) + n > VID_FIFO - 2) /* demasiado acumulado: tirar lo mas viejo */
        vid_r = vid_w + (u32)n - (VID_FIFO / 2);
    for (i = 0; i < n; i++) {
        s16 v = vid_bits == 16 ? ((const s16 *)data)[i] : (s16)((((const u8 *)data)[i] - 128) << 8);
        vid_fifo[vid_w++ & (VID_FIFO - 1)] = v;
    }
    nds_audio_update();
}

void nds_vid_audio_end(void)
{
    nds_mus_stop();
    free(vid_fifo);
    vid_fifo = NULL;
}


/* ---------- cinematicas propias (NDS-DSV) ---------- */
/* Archivo .dsv: cabecera de 32 bytes ("DSV1", ancho, alto, fotogramas, us por fotograma, Hz, bits, canales),
   paleta de 256 colores RGB15, y por cada fotograma: u32 largo, u16 bytes de audio, audio, imagen
   (pares u16 saltar / u16 copiar en bloques de 8 pixeles, respecto al fotograma anterior). */
#define DSV_REC_MAX (2 + 65536 + 4 * 6144 + LCD_W * LCD_H)
static FILE *dsv_f;
static u8 *dsv_frame, *dsv_rec;
static u32 dsv_n, dsv_i, dsv_us, dsv_t0, dsv_rate, dsv_lastshow;
static int dsv_loop;
static long dsv_data_off;
static u16 dsv_pal[256];

static void dsv_free(void)
{
    if (dsv_f)
        fclose(dsv_f);
    dsv_f = NULL;
    free(dsv_frame);
    free(dsv_rec);
    dsv_frame = dsv_rec = NULL;
}

static void dsv_palette(void)
{
    int i;
    for (i = 0; i < 256; i++)
        BG_PALETTE[i] = dsv_pal[i];
}

/* NDS-CINES: lista de cinematicas vistas en saves/cinematicas.txt */
int nds_cine_seen(const char *base)
{
    char line[48];
    int found = 0;
    FILE *f = fopen(nds_r("/devilutionx/saves/cinematicas.txt"), "r");
    if (!f)
        return 0;
    while (!found && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = 0;
        if (strcmp(line, base) == 0)
            found = 1;
    }
    fclose(f);
    return found;
}
void nds_extra_mark(const char *name) /* NDS-MUSICAS: apuntar sin filtro (musicas) */
{
    FILE *f;
    if (!name || !*name || nds_cine_seen(name))
        return;
    f = fopen(nds_r("/devilutionx/saves/cinematicas.txt"), "a");
    if (f) {
        fprintf(f, "%s\n", name);
        fclose(f);
    }
}
static void nds_cine_mark(const char *base)
{
    FILE *f;
    if (strcmp(base, "fprst3") != 0 && strncmp(base, "diabvic", 7) != 0 && strcmp(base, "diabend") != 0)
        return;
    if (nds_cine_seen(base))
        return;
    f = fopen(nds_r("/devilutionx/saves/cinematicas.txt"), "a");
    if (f) {
        fprintf(f, "%s\n", base);
        fclose(f);
    }
}

int nds_dsv_begin(const char *name, int loop)
{
    char base[40], path[96];
    const char *p = name, *q;
    size_t n = 0;
    u8 h[32];
    for (q = name; *q; q++)
        if (*q == '\\' || *q == '/')
            p = q + 1;
    while (p[n] && p[n] != '.' && n < sizeof base - 1) {
        char c = p[n];
        base[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        n++;
    }
    base[n] = 0;
    nds_cine_mark(base); /* NDS-CINES */
    snprintf(path, sizeof path, nds_r("/devilutionx/dsv/%s.dsv"), base);
    dsv_f = fopen(path, "rb");
    if (!dsv_f)
        return 0;
    setvbuf(dsv_f, NULL, _IOFBF, 32768);
    if (fread(h, 1, 32, dsv_f) != 32 || memcmp(h, "DSV1", 4) != 0 || (h[4] | h[5] << 8) != LCD_W
        || (h[6] | h[7] << 8) != LCD_H || fread(dsv_pal, 2, 256, dsv_f) != 256) {
        dsv_free();
        return 0;
    }
    dsv_n = h[8] | h[9] << 8 | h[10] << 16 | (u32)h[11] << 24;
    dsv_us = h[12] | h[13] << 8 | h[14] << 16 | (u32)h[15] << 24;
    dsv_rate = h[16] | h[17] << 8 | h[18] << 16 | (u32)h[19] << 24;
    dsv_frame = (u8 *)calloc(1, LCD_W * LCD_H);
    dsv_rec = (u8 *)malloc(DSV_REC_MAX);
    if (!dsv_frame || !dsv_rec || dsv_n == 0 || dsv_us == 0) {
        dsv_free();
        return 0;
    }
    dsv_data_off = ftell(dsv_f);
    dsv_loop = loop;
    dsv_i = 0;
    dsv_palette();
    if (dsv_rate)
        nds_vid_audio_begin((int)dsv_rate, 1, 8, 120);
    dsv_t0 = SDL_GetTicks();
    dsv_lastshow = 0;
    return 1;
}

static int dsv_read_frame(void)
{
    u32 len, k, pos = 0;
    u8 lb[4];
    if (fread(lb, 1, 4, dsv_f) != 4)
        return 0;
    len = lb[0] | lb[1] << 8 | lb[2] << 16 | (u32)lb[3] << 24;
    if (len < 2 || len > DSV_REC_MAX || fread(dsv_rec, 1, len, dsv_f) != len)
        return 0;
    k = dsv_rec[0] | dsv_rec[1] << 8;
    if (2 + k > len)
        return 0;
    if (k && dsv_rate)
        nds_vid_audio_push(dsv_rec + 2, (int)k);
    k += 2;
    while (pos < 6144) {
        u32 skip, copy;
        if (k + 4 > len)
            return 0;
        skip = dsv_rec[k] | dsv_rec[k + 1] << 8;
        copy = dsv_rec[k + 2] | dsv_rec[k + 3] << 8;
        k += 4;
        pos += skip;
        if (pos + copy > 6144 || k + copy * 8 > len)
            return 0;
        memcpy(dsv_frame + pos * 8, dsv_rec + k, copy * 8);
        k += copy * 8;
        pos += copy;
    }
    return 1;
}

int nds_dsv_continue(void)
{
    u32 now, due;
    if (!dsv_f)
        return 0;
    if (dsv_i >= dsv_n) {
        if (!dsv_loop) { /* NDS-DSVFIN: dejar que termine de sonar lo que queda en cola (el grito final) antes de cerrar */
            static u32 fin_t0, fin_vacio;
            if (dsv_rate && vid_fifo) {
                const u32 ahora = SDL_GetTicks();
                if (!fin_t0) {
                    fin_t0 = ahora;
                    fin_vacio = 0;
                }
                if ((s32)(vid_w - vid_r) > 0)
                    fin_vacio = 0;
                else if (!fin_vacio)
                    fin_vacio = ahora;
                if (ahora - fin_t0 < 5000 && (!fin_vacio || ahora - fin_vacio < 300)) {
                    SDL_Delay(5);
                    return 1;
                }
            }
            fin_t0 = 0;
            return 0;
        }
        fseek(dsv_f, dsv_data_off, SEEK_SET);
        memset(dsv_frame, 0, LCD_W * LCD_H);
        dsv_i = 0;
        dsv_t0 = SDL_GetTicks();
    }
    if (dsv_rate && vid_fifo) { /* NDS-DSVSYNC: el ritmo lo marca el sonido (cola de unos 7 fotogramas) */
        const s32 fpf = (s32)((unsigned long long)dsv_rate * dsv_us / 1000000u);
        if ((s32)(vid_w - vid_r) > fpf * 1) { /* NDS-DSVMARGEN: cola de 1 fotograma (el resto del adelanto esta en el anillo) */
            SDL_Delay(2);
            return 1;
        }
    } else {
        now = SDL_GetTicks();
        due = dsv_t0 + (u32)((unsigned long long)dsv_i * dsv_us / 1000u);
        if ((s32)(now - due) < 0) { /* aun no toca: esperar un poco (el juego sigue mirando los botones) */
            u32 w = due - now;
            SDL_Delay(w > 5 ? 5 : w);
            return 1;
        }
    }
    if (!dsv_read_frame())
        return 0;
    dsv_i++;
    /* se muestra si no vamos con retraso (o si hace 4 fotogramas que no se muestra ninguno) */
    due = dsv_t0 + (u32)((unsigned long long)dsv_i * dsv_us / 1000u);
    {
        int vamos_bien;
        if (dsv_rate && vid_fifo) { /* NDS-DSVSYNC: con poco sonido en cola vamos atrasados: saltar la imagen */
            const s32 fpf = (s32)((unsigned long long)dsv_rate * dsv_us / 1000000u);
            vamos_bien = (s32)(vid_w - vid_r) > 0; /* NDS-DSVMARGEN */
        } else
            vamos_bien = (s32)(SDL_GetTicks() - due) < 0;
        if (!vamos_bien && dsv_i - dsv_lastshow < 4)
            return 1;
    }
    {
        dsv_lastshow = dsv_i;
        nds_ui_frame_scaled(dsv_frame, LCD_W, LCD_W, LCD_H);
        SDL_Flip(&video_surface);
    }
    return 1;
}

void nds_dsv_end(void)
{
    if (dsv_rate)
        nds_vid_audio_end();
    dsv_rate = 0;
    dsv_free();
    nds_ui_frame_scaled(NULL, 0, 0, 0);
}
