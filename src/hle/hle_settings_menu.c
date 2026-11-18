/*
 * hle_settings_menu.c -- the desktop's in-game settings menu.
 *
 * F2 on the keyboard, or Start+Back together on a pad, opens a panel over
 * the game (drawn by the renderer's overlay, d3d8_overlay_set_menu) with the
 * settings a player changes most: the frame-rate cap, frame interpolation,
 * the frame-rate counter and fullscreen apply at once; the rest (internal
 * resolution, widescreen, texture filtering, variable refresh, and the
 * title's own lines such as the movies) are written to the settings file and
 * take effect at the next start, which the panel says.
 *
 * Every change is written to the settings file as it is made
 * (recomp_config_set), so the launcher and the next run see it.
 *
 * Input. The menu reads port 1 through the same path the game does
 * (src/hle/input_host.c calls hle_settings_menu_input with every sample), so
 * a pad and the keyboard's default bindings both drive it: D-pad or arrows
 * choose, left/right change, A (Z) changes too, B (X), Back (Backspace), F2
 * or Esc close. While it is open every port reads as untouched, so nothing
 * reaches the game, and the Start+Back that opened or closed it is held back
 * from the game until both are released.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hle_settings.h"
#include "input_model.h"
#include "input_host.h"
#include "recomp_config.h"


enum {
    PAD_UP = 0x0001u, PAD_DOWN = 0x0002u, PAD_LEFT = 0x0004u, PAD_RIGHT = 0x0008u,
    PAD_START = 0x0010u, PAD_BACK = 0x0020u,
};

enum { LIVE_NONE, LIVE_CAP, LIVE_INTERP, LIVE_FPS, LIVE_FULLSCREEN };

typedef struct {
    const char *label;
    const char *key;                     /* the settings file's key */
    const char *env;                     /* the variable that overrides it */
    int live;                            /* how it applies now, or LIVE_NONE */
    int n;
    const char *vals[8];                 /* as written to the file */
    const char *names[8];                /* as shown */
} Item;

static const Item ITEMS[] = {
    { "Frame rate cap",      "frame_cap",        "RECOMP_FPS_CAP",       LIVE_CAP, 4,
      { "adaptive", "60", "30", "0" }, { "adaptive", "60", "30", "off" } },
    { "Frame interpolation", "frame_interp",     "RECOMP_FRAME_INTERP",  LIVE_INTERP, 4,
      { "1", "2", "3", "4" }, { "off", "2x", "3x", "4x" } },
    { "Frame-rate counter",  "fps_overlay",      "RECOMP_FPS_OVERLAY",   LIVE_FPS, 2,
      { "0", "1" }, { "off", "on" } },
    { "Fullscreen",          "fullscreen",       "RECOMP_FULLSCREEN",    LIVE_FULLSCREEN, 2,
      { "0", "1" }, { "off", "on" } },
    { "Internal resolution", "resolution_scale", "RECOMP_RES_SCALE",     LIVE_NONE, 8,
      { "1", "2", "3", "4", "5", "6", "7", "8" }, { "1x", "2x", "3x", "4x", "5x", "6x", "7x", "8x" } },
    { "Widescreen (16:9)",   "widescreen",       "RECOMP_WIDESCREEN",    LIVE_NONE, 2,
      { "0", "1" }, { "off", "on" } },
    { "Texture sharpness",   "anisotropy",       "RECOMP_ANISO",         LIVE_NONE, 6,
      { "0", "1", "2", "4", "8", "16" }, { "off", "1x", "2x", "4x", "8x", "16x" } },
    { "Variable refresh",    "vrr",              "RECOMP_VRR",           LIVE_NONE, 2,
      { "0", "1" }, { "off", "on" } },
};
#define ITEM_COUNT ((int)(sizeof ITEMS / sizeof ITEMS[0]))

/* The title's own lines, if its settings header has them: movies and sound.
 * Shown only when the file (or the defaults) know the key. */
static const Item TITLE_ITEMS[] = {
    { "Movies",              "skip_intro",       NULL,                   LIVE_NONE, 3,
      { "1", "intro", "0" }, { "skip all", "skip intro", "play all" } },
    { "Sound",               "mute",             "RECOMP_MUTE",          LIVE_NONE, 2,
      { "0", "1" }, { "on", "off" } },
};
#define TITLE_ITEM_COUNT ((int)(sizeof TITLE_ITEMS / sizeof TITLE_ITEMS[0]))

static volatile int g_open;
static int  g_sel;
static int  g_restart_needed;
static int  g_swallow;                   /* Start+Back still down after a toggle */
static unsigned g_prev_buttons;
static int  g_prev_a, g_prev_b;
static const Item *g_items[ITEM_COUNT + TITLE_ITEM_COUNT];
static int  g_count;
static char g_text[2048];

static void build_list(void)
{
    int i;

    if (g_count)
        return;
    for (i = 0; i < ITEM_COUNT; i++)
        g_items[g_count++] = &ITEMS[i];
    for (i = 0; i < TITLE_ITEM_COUNT; i++)
        if (recomp_config_lookup(NULL, TITLE_ITEMS[i].key))
            g_items[g_count++] = &TITLE_ITEMS[i];
}

/* The value shown: what is in effect for a live setting, the file's (or the
 * default) for the rest. */
static int current_index(const Item *it)
{
    char buf[16];
    const char *v;
    int i;

    switch (it->live) {
    case LIVE_CAP:
        v = hle_frame_cap();
        for (i = 0; i < it->n; i++)
            if (strcmp(v, it->names[i]) == 0)
                return i;
        return 0;
    case LIVE_INTERP:
        snprintf(buf, sizeof buf, "%d", hle_frame_interp());
        v = buf;
        break;
    case LIVE_FPS:
        v = hle_fps_overlay() ? "1" : "0";
        break;
    case LIVE_FULLSCREEN:
        v = hle_fullscreen() ? "1" : "0";
        break;
    default:
        v = recomp_config_lookup(it->env, it->key);
        if (!v || !*v)
            v = strcmp(it->key, "resolution_scale") == 0 ? "1" : it->vals[0];
        break;
    }
    for (i = 0; i < it->n; i++)
        if (strcmp(v, it->vals[i]) == 0)
            return i;
    return 0;
}

static void change(int dir)
{
    const Item *it;
    int i;

    if (g_sel >= g_count)                /* "Close" */
        return;
    it = g_items[g_sel];
    i = (current_index(it) + dir + it->n) % it->n;
    recomp_config_set(it->key, it->vals[i]);
    switch (it->live) {
    case LIVE_CAP:        hle_set_frame_cap(it->names[i]); break;
    case LIVE_INTERP:     hle_set_frame_interp(atoi(it->vals[i])); break;
    case LIVE_FPS:        hle_set_fps_overlay(i); break;
    case LIVE_FULLSCREEN: hle_set_fullscreen(i); break;
    default:              g_restart_needed = 1; break;
    }
    fprintf(stderr, "[MENU] %s = %s%s\n", it->key, it->vals[i],
            it->live == LIVE_NONE ? " (at the next start)" : "");
    fflush(stderr);
}

int hle_settings_menu_open(void)
{
    return g_open;
}

void hle_settings_menu_toggle(void)
{
    build_list();
    g_open = !g_open;
    fprintf(stderr, "[MENU] settings menu %s\n", g_open ? "opened" : "closed");
    fflush(stderr);
}

const char *hle_settings_menu_text(void)
{
    int i, o = 0;

    if (!g_open)
        return NULL;
    build_list();
#define ADD(...) do { if (o < (int)sizeof g_text) \
        o += snprintf(g_text + o, sizeof g_text - (size_t)o, __VA_ARGS__); } while (0)
    ADD("            SETTINGS\n\n");
    for (i = 0; i < g_count; i++) {
        const Item *it = g_items[i];
        int env = it->env && getenv(it->env) && *getenv(it->env) && it->live == LIVE_NONE;
        ADD("%s%s %-20s < %-10s >%s\n", i == g_sel ? "\x01" : "", i == g_sel ? ">" : " ",
            it->label, it->names[current_index(it)],
            it->live == LIVE_NONE ? (env ? " *env" : " *") : "");
    }
    ADD("%s%s Close\n\n", g_sel == g_count ? "\x01" : "", g_sel == g_count ? ">" : " ");
    ADD("  * at the next start%s\n", g_restart_needed ? ": restart to apply" : "");
    ADD("  Up/Down choose   Left/Right/A change   B/F2 close");
#undef ADD
    return g_text;
}

void hle_settings_menu_keys(int dy, int dx, int ok, int back)
{
    if (!g_open)
        return;
    if (back) {
        hle_settings_menu_toggle();
        return;
    }
    if (dy)
        g_sel = (g_sel + dy + g_count + 1) % (g_count + 1);
    if (dx)
        change(dx);
    if (ok) {
        if (g_sel == g_count)
            hle_settings_menu_toggle();
        else
            change(1);
    }
}

static long now_ms(void)
{
    return (long)((double)clock() * 1000.0 / CLOCKS_PER_SEC);
}

void hle_settings_menu_input(unsigned port, RecompInputGamepad *g)
{
    /* Start and Back are held back from the game for a moment when one is
     * pressed alone, so that pressing them together (never quite at once)
     * does not first pause the game with the one that landed first. */
    static unsigned held_bits;
    static long held_since;

    if (!g)
        return;
    if (port == 0) {
        unsigned b = g->buttons, pressed = b & ~g_prev_buttons;
        int a = g->analog_buttons[HOST_ANALOG_A] > 30, bb = g->analog_buttons[HOST_ANALOG_B] > 30;
        int stick_y = g->thumb_ly > 16000 ? -1 : g->thumb_ly < -16000 ? 1 : 0;
        int stick_x = g->thumb_lx > 16000 ? 1 : g->thumb_lx < -16000 ? -1 : 0;
        static int prev_sy, prev_sx;
        const unsigned combo = PAD_START | PAD_BACK;

        build_list();
        if ((b & combo) == combo && (pressed & combo) && !g_swallow) {
            hle_settings_menu_toggle();
            g_swallow = 1;
            held_bits = 0;
        } else if (g_open && !g_swallow) {
            int dy = (pressed & PAD_UP) ? -1 : (pressed & PAD_DOWN) ? 1 : 0;
            int dx = (pressed & PAD_LEFT) ? -1 : (pressed & PAD_RIGHT) ? 1 : 0;
            int back = (bb && !g_prev_b) || (pressed & PAD_BACK) != 0;

            if (!dy && stick_y != prev_sy) dy = stick_y;
            if (!dx && stick_x != prev_sx) dx = stick_x;
            hle_settings_menu_keys(dy, dx, a && !g_prev_a, back);
            if (back)
                g_swallow = 1;           /* Back (or Start) still down: not the game's */
        } else if (!g_open && !g_swallow && (pressed & combo)) {
            held_bits = pressed & combo;
            held_since = now_ms();
        }
        if (g_swallow && !(b & combo))
            g_swallow = 0;
        if (held_bits) {
            if (!(b & held_bits)) {
                /* A tap shorter than the wait: the game still gets it, for
                 * this sample, and the release with the next. */
                g->buttons |= (uint16_t)held_bits;
                held_bits = 0;
            } else if (now_ms() - held_since > 150) {
                held_bits = 0;           /* held long enough: the game's from now on */
            } else {
                g->buttons &= (uint16_t)~held_bits;
            }
        }
        g_prev_buttons = b;
        g_prev_a = a;
        g_prev_b = bb;
        prev_sy = stick_y;
        prev_sx = stick_x;
    }
    if (g_open || g_swallow)
        memset(g, 0, sizeof *g);         /* the game sees nothing pressed */
}
