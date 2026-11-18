/*
 * recomp_pad.c -- the host's game pads through SDL3, or XInput on Windows.
 * recomp_pad.h says what this is for; the notes here are about how.
 *
 * SDL3 is started with the gamepad subsystem only: no window, no event
 * loop. So nothing here waits for SDL events. A thread of the pads' own
 * (sdl_pad_thread) re-reads the pad list every SCAN_MS and refreshes pad
 * state every 2 ms; a title's polls copy the last state. The joystick and
 * gamepad event types are switched off so an event queue nobody drains
 * cannot grow. On Windows SDL's raw-input work runs on its own thread
 * besides (SDL_HINT_JOYSTICK_THREAD).
 */
#include "recomp_pad.h"
#include "recomp_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <xinput.h>
#endif

#define SCAN_MS       250u     /* how often the SDL3 pad list is re-read */
#define RUMBLE_MS   30000u     /* SDL rumble has a duration; XInput's does not */

static int g_api = -1;         /* resolved on first use */
static int g_api_asked = RECOMP_PAD_API_SDL;

static int env_api(void)
{
    const char *v = getenv("RECOMP_PAD_API");
    if (!v || !*v)
        return -1;
    if (SDL_strcasecmp(v, "xinput") == 0)
        return RECOMP_PAD_API_XINPUT;
    if (SDL_strcasecmp(v, "sdl") == 0 || SDL_strcasecmp(v, "sdl3") == 0)
        return RECOMP_PAD_API_SDL;
    fprintf(stderr, "[INPUT] RECOMP_PAD_API=%s is not sdl or xinput; ignored\n", v);
    return -1;
}

void recomp_pad_set_api(int api)
{
    g_api_asked = api == RECOMP_PAD_API_XINPUT ? RECOMP_PAD_API_XINPUT : RECOMP_PAD_API_SDL;
    g_api = -1;         /* resolved again on the next read: the launcher flips it live */
}

/* ---- SDL3 ------------------------------------------------------------- */

/* Everything SDL is done on one thread of the pads' own, started by the
 * first read and kept for the life of the process.
 *
 * - Start-up enumerates every input device, and on some machines that takes
 *   seconds: 20.8 s once on a Windows 11 PC with an Xbox One pad, instant on
 *   the next tries (a device waking up). The title's first pad poll comes
 *   before its first frame, so it used to wait for all of it on a black
 *   screen. Until the thread is up there are no SDL pads; a pad found later
 *   arrives as an insertion, as one plugged in after start-up always has.
 * - Hot-plug. SDL learns of a new device through hidden windows (the HIDAPI
 *   discovery window, device notifications), and a window's messages reach
 *   only the thread that made it. Polled from whichever title thread asked,
 *   or started on a thread that then ended, a pad plugged in after start-up
 *   was never seen. This thread made them, pumps its messages, and updates
 *   and rescans the pads itself.
 *
 * The title's threads only copy the last state out (POLL_NS old at most). */

#define POLL_NS     2000000ull  /* 2 ms between updates */

typedef struct {
    SDL_Gamepad   *pad;          /* the pad thread's only */
    SDL_JoystickID id;
    uint64_t       rumble_at;
    uint64_t       used_at;      /* when it was last used, in uses: 0 never */
    RecompPadState last;
} SdlSlot;

/* pad_follow_input (RECOMP_PAD_FOLLOW_INPUT, on unless 0): controller 1 is
 * whichever pad was used last -- a button pressed, a trigger pulled or a
 * stick pushed -- and the other pads follow in the order they were found.
 * Off, each pad keeps the slot it was found in (for games with several
 * players). Read where the thread starts. */
static int g_follow = 1;
static uint64_t g_uses;

/* A pad used since the last look: a new button, or a trigger or stick that
 * crossed half-way. */
static int sdl_used(const RecompPadState *was, const RecompPadState *now)
{
#define PAST(v) ((v) > 16000 || (v) < -16000)
    if (now->buttons & ~was->buttons)
        return 1;
    if ((now->left_trigger > 128 && was->left_trigger <= 128) ||
        (now->right_trigger > 128 && was->right_trigger <= 128))
        return 1;
    return (PAST(now->lx) && !PAST(was->lx)) || (PAST(now->ly) && !PAST(was->ly)) ||
           (PAST(now->rx) && !PAST(was->rx)) || (PAST(now->ry) && !PAST(was->ry));
#undef PAST
}

typedef struct {                 /* published under g_lock */
    int            have;
    RecompPadState st;
    char           name[64];
    uint16_t       rumble_low, rumble_high;
    int            rumble_new;
} SdlShared;

static SdlSlot         g_sdl[RECOMP_PAD_SLOTS];
static SdlShared       g_pub[RECOMP_PAD_SLOTS];
static SDL_SpinLock    g_lock;
/* 0 not tried, 2 starting, 1 running, -1 failed. */
static SDL_AtomicInt   g_sdl_state;

/* Drops pads that went away and seats new ones in the first free slot. */
static void sdl_scan(void)
{
    SDL_JoystickID *ids;
    int n = 0, i, s;

    for (s = 0; s < RECOMP_PAD_SLOTS; s++)
        if (g_sdl[s].pad && !SDL_GamepadConnected(g_sdl[s].pad)) {
            fprintf(stderr, "[INPUT] pad %d disconnected\n", s + 1);
            SDL_CloseGamepad(g_sdl[s].pad);
            memset(&g_sdl[s], 0, sizeof g_sdl[s]);
        }
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n; i++) {
        int seated = 0, free_slot = -1;
        for (s = 0; s < RECOMP_PAD_SLOTS; s++) {
            if (g_sdl[s].pad && g_sdl[s].id == ids[i])
                seated = 1;
            else if (!g_sdl[s].pad && free_slot < 0)
                free_slot = s;
        }
        if (seated || free_slot < 0)
            continue;
        g_sdl[free_slot].pad = SDL_OpenGamepad(ids[i]);
        if (g_sdl[free_slot].pad) {
            const char *name = SDL_GetGamepadName(g_sdl[free_slot].pad);
            g_sdl[free_slot].id = ids[i];
            fprintf(stderr, "[INPUT] pad %d: %s\n", free_slot + 1, name ? name : "(unnamed)");
        }
    }
    SDL_free(ids);
    fflush(stderr);
}

static int16_t flip_y(Sint16 v)
{
    return v == -32768 ? (int16_t)32767 : (int16_t)-v;
}

static void sdl_state_of(SDL_Gamepad *p, RecompPadState *out)
{
    uint16_t b = 0;

    memset(out, 0, sizeof *out);
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP))        b |= RECOMP_PAD_DPAD_UP;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN))      b |= RECOMP_PAD_DPAD_DOWN;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT))      b |= RECOMP_PAD_DPAD_LEFT;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))     b |= RECOMP_PAD_DPAD_RIGHT;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_START))          b |= RECOMP_PAD_START;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_BACK))           b |= RECOMP_PAD_BACK;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_STICK))     b |= RECOMP_PAD_LEFT_THUMB;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_STICK))    b |= RECOMP_PAD_RIGHT_THUMB;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  b |= RECOMP_PAD_LEFT_SHOULDER;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) b |= RECOMP_PAD_RIGHT_SHOULDER;
    /* By position, as SDL3 names them: the bottom face button is A on an
     * Xbox pad, Cross on a PlayStation one and B on a Nintendo one, and it
     * is the button a title asking for A expects under the right thumb. */
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH))          b |= RECOMP_PAD_A;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST))           b |= RECOMP_PAD_B;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_WEST))           b |= RECOMP_PAD_X;
    if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_NORTH))          b |= RECOMP_PAD_Y;
    out->buttons = b;
    /* Triggers are 0..32767 in SDL, 0..255 in XInput. */
    out->left_trigger  = (uint8_t)(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
    out->right_trigger = (uint8_t)(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);
    /* SDL's Y axes point down and XInput's up. Negated, with -32768 (full
     * down in SDL) clamped to 32767 so it does not overflow, and 0 staying 0
     * at rest. */
    out->lx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX);
    out->ly = flip_y(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY));
    out->rx = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX);
    out->ry = flip_y(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY));
}

static int SDLCALL sdl_pad_thread(void *unused)
{
    Uint32 t;
    int v, s;
    Uint64 began = SDL_GetTicks(), scan_at = 0;

    (void)unused;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[INPUT] SDL3 could not start its gamepad support (%s)\n",
                SDL_GetError());
        fflush(stderr);
        SDL_SetAtomicInt(&g_sdl_state, -1);
        return 0;
    }
    for (t = SDL_EVENT_JOYSTICK_AXIS_MOTION; t < SDL_EVENT_FINGER_DOWN; t++)
        SDL_SetEventEnabled(t, false);
    v = SDL_GetVersion();
    fprintf(stderr, "[INPUT] pads through SDL3 %d.%d.%d, ready after %.2f s "
                    "(RECOMP_PAD_API=xinput for XInput)\n",
            SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v),
            (double)(SDL_GetTicks() - began) / 1000.0);
    fflush(stderr);
    SDL_SetAtomicInt(&g_sdl_state, 1);

    for (;;) {
        SdlShared now_pub[RECOMP_PAD_SLOTS];
        Uint64 now;

#if defined(_WIN32)
        {   /* the hidden windows SDL made on this thread: device arrivals */
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
        }
#endif
        SDL_UpdateGamepads();
        now = SDL_GetTicks();
        if (now >= scan_at) {
            sdl_scan();
            scan_at = now + SCAN_MS;
        }
        {
            /* order[k]: the pad behind controller slot k */
            int order[RECOMP_PAD_SLOTS], n = 0, first = -1, k;
            static int said_first = -1;

            for (s = 0; s < RECOMP_PAD_SLOTS; s++) {
                RecompPadState st;
                if (!g_sdl[s].pad)
                    continue;
                sdl_state_of(g_sdl[s].pad, &st);
                if (sdl_used(&g_sdl[s].last, &st))
                    g_sdl[s].used_at = ++g_uses;
                g_sdl[s].last = st;
            }
            if (g_follow)
                for (s = 0; s < RECOMP_PAD_SLOTS; s++)
                    if (g_sdl[s].pad && g_sdl[s].used_at &&
                        (first < 0 || g_sdl[s].used_at > g_sdl[first].used_at))
                        first = s;
            if (first >= 0)
                order[n++] = first;
            for (s = 0; s < RECOMP_PAD_SLOTS; s++)
                if (s != first)
                    order[n++] = s;
            if (first >= 0 && first != said_first) {
                const char *name = SDL_GetGamepadName(g_sdl[first].pad);
                said_first = first;
                fprintf(stderr, "[INPUT] controller 1 follows the pad used last: %s (pad %d)\n",
                        name ? name : "Gamepad", first + 1);
                fflush(stderr);
            }

            memset(now_pub, 0, sizeof now_pub);
            for (k = 0; k < RECOMP_PAD_SLOTS; k++) {
                const char *name;
                s = order[k];
                if (!g_sdl[s].pad)
                    continue;
                now_pub[k].have = 1;
                now_pub[k].st = g_sdl[s].last;
                name = SDL_GetGamepadName(g_sdl[s].pad);
                snprintf(now_pub[k].name, sizeof now_pub[k].name, "%s", name ? name : "Gamepad");
            }
            SDL_LockSpinlock(&g_lock);
            for (k = 0; k < RECOMP_PAD_SLOTS; k++) {
                uint16_t lo = g_pub[k].rumble_low, hi = g_pub[k].rumble_high;
                int fresh = g_pub[k].rumble_new;

                s = order[k];
                g_pub[k].rumble_new = 0;
                g_pub[k].have = now_pub[k].have;
                g_pub[k].st = now_pub[k].st;
                memcpy(g_pub[k].name, now_pub[k].name, sizeof g_pub[k].name);
                SDL_UnlockSpinlock(&g_lock);
                /* SDL rumble has a duration; XInput's does not: renewed while on. */
                if (g_sdl[s].pad && (fresh || ((lo || hi) && now - g_sdl[s].rumble_at > RUMBLE_MS / 2))) {
                    SDL_RumbleGamepad(g_sdl[s].pad, lo, hi, (lo || hi) ? RUMBLE_MS : 0);
                    g_sdl[s].rumble_at = now;
                }
                SDL_LockSpinlock(&g_lock);
            }
        }
        SDL_UnlockSpinlock(&g_lock);
        SDL_DelayNS(POLL_NS);
    }
    return 0;
}

/* TRUE once SDL's gamepad support is up; the first call starts it. */
static int sdl_start(void)
{
    int st = SDL_GetAtomicInt(&g_sdl_state);
    SDL_Thread *th;

    if (st == 1)
        return 1;
    if (st != 0 || !SDL_CompareAndSwapAtomicInt(&g_sdl_state, 0, 2))
        return 0;
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    /* The window the player looks at may not be SDL's, so SDL cannot know
     * it has focus: read the pads regardless, as XInput always did. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#if defined(_WIN32)
    /* SDL's DirectInput back end enumerates every DirectInput device, and on
     * a Windows 11 PC that took 40 s every time (0.01 s without it). Xbox
     * pads come through XInput, raw input and WGI, PlayStation and Switch
     * pads through HIDAPI; only an old DirectInput-only pad needs it:
     * pad_directinput = 1 (RECOMP_PAD_DIRECTINPUT=1). */
    if (!recomp_config_bool("RECOMP_PAD_DIRECTINPUT", "pad_directinput", 0))
        SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
#endif
    g_follow = recomp_config_bool("RECOMP_PAD_FOLLOW_INPUT", "pad_follow_input", 1);
#ifndef _WIN32
    /* The gamepad subsystem brings up SDL's events, which would otherwise
     * turn SIGINT/SIGTERM into a quit event nobody reads: a timeout or
     * Ctrl-C must still end the process (src/host/host_sdl.c does the same).
     * Windows keeps its behaviour as it was. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#endif
    th = SDL_CreateThread(sdl_pad_thread, "recomp_pads", NULL);
    if (!th) {
        fprintf(stderr, "[INPUT] no thread for the pads (%s): SDL3 pads off\n", SDL_GetError());
        fflush(stderr);
        SDL_SetAtomicInt(&g_sdl_state, -1);
        return 0;
    }
    SDL_DetachThread(th);
    return 0;
}

static int sdl_read(int slot, RecompPadState *out)
{
    int have;

    if (!sdl_start())
        return 0;
    SDL_LockSpinlock(&g_lock);
    have = g_pub[slot].have;
    if (have)
        *out = g_pub[slot].st;
    SDL_UnlockSpinlock(&g_lock);
    return have;
}

/* ---- XInput (Windows) ----------------------------------------------------- */

#if defined(_WIN32)

/* XInputGetState on an empty slot costs about a millisecond, and a title
 * polls its pads several times a frame: an unplugged port would cost more
 * than the game. An empty slot is re-checked once a second, a full one
 * every poll. */
static int xinput_read(int slot, RecompPadState *out)
{
    static int connected[RECOMP_PAD_SLOTS];
    static ULONGLONG retry_at[RECOMP_PAD_SLOTS];
    ULONGLONG now = GetTickCount64();
    XINPUT_STATE st;

    if (!connected[slot] && now < retry_at[slot])
        return 0;
    memset(&st, 0, sizeof st);
    if (XInputGetState((DWORD)slot, &st) != ERROR_SUCCESS) {
        connected[slot] = 0;
        retry_at[slot] = now + 1000;
        return 0;
    }
    connected[slot] = 1;
    out->buttons = st.Gamepad.wButtons;
    out->left_trigger = st.Gamepad.bLeftTrigger;
    out->right_trigger = st.Gamepad.bRightTrigger;
    out->lx = st.Gamepad.sThumbLX;
    out->ly = st.Gamepad.sThumbLY;
    out->rx = st.Gamepad.sThumbRX;
    out->ry = st.Gamepad.sThumbRY;
    return 1;
}

#endif

/* ---- the interface ---------------------------------------------------------- */

int recomp_pad_api(void)
{
    if (g_api < 0) {
        int e = env_api();
        g_api = e >= 0 ? e : g_api_asked;
#if !defined(_WIN32)
        g_api = RECOMP_PAD_API_SDL;
#endif
        if (g_api == RECOMP_PAD_API_XINPUT) {
            fprintf(stderr, "[INPUT] pads through XInput (Xbox controllers only)\n");
            fflush(stderr);
        }
    }
    return g_api;
}

const char *recomp_pad_api_name(void)
{
    return recomp_pad_api() == RECOMP_PAD_API_XINPUT ? "XInput" : "SDL3";
}

int recomp_pad_read(int slot, RecompPadState *out)
{
    if (!out)
        return 0;
    memset(out, 0, sizeof *out);
    if (slot < 0 || slot >= RECOMP_PAD_SLOTS)
        return 0;
#if defined(_WIN32)
    if (recomp_pad_api() == RECOMP_PAD_API_XINPUT)
        return xinput_read(slot, out);
#else
    (void)recomp_pad_api();
#endif
    return sdl_read(slot, out);
}

void recomp_pad_name(int slot, char *out, size_t n)
{
    if (!out || !n)
        return;
    out[0] = '\0';
    if (slot < 0 || slot >= RECOMP_PAD_SLOTS)
        return;
#if defined(_WIN32)
    if (recomp_pad_api() == RECOMP_PAD_API_XINPUT) {
        RecompPadState st;
        if (xinput_read(slot, &st))
            snprintf(out, n, "Xbox controller");
        return;
    }
#endif
    {
        RecompPadState st;
        if (!sdl_read(slot, &st))
            return;
        SDL_LockSpinlock(&g_lock);
        snprintf(out, n, "%s", g_pub[slot].name[0] ? g_pub[slot].name : "Gamepad");
        SDL_UnlockSpinlock(&g_lock);
    }
}

void recomp_pad_rumble(int slot, uint16_t low, uint16_t high)
{
    if (slot < 0 || slot >= RECOMP_PAD_SLOTS)
        return;
#if defined(_WIN32)
    if (recomp_pad_api() == RECOMP_PAD_API_XINPUT) {
        XINPUT_VIBRATION v;
        v.wLeftMotorSpeed = low;
        v.wRightMotorSpeed = high;
        XInputSetState((DWORD)slot, &v);
        return;
    }
#endif
    if (!sdl_start())
        return;
    SDL_LockSpinlock(&g_lock);
    g_pub[slot].rumble_low = low;
    g_pub[slot].rumble_high = high;
    g_pub[slot].rumble_new = 1;
    SDL_UnlockSpinlock(&g_lock);
}
