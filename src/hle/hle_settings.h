/*
 * hle_settings.h - the settings a player can change while a title runs.
 *
 * What the desktop's F9 and F10 do, for a host with no keyboard: Android's
 * in-game menu (src/host/android_main.c) calls these. Each takes effect at
 * the next frame and needs no restart. Any thread.
 */
#ifndef XBOXRECOMP_HLE_SETTINGS_H
#define XBOXRECOMP_HLE_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The frame-rate counter in the top-left corner (F9). */
int  hle_fps_overlay(void);
void hle_set_fps_overlay(int on);

/* The frame cap (F10): "adaptive", "60", "30" or "off". */
const char *hle_frame_cap(void);
void        hle_set_frame_cap(const char *name);

/* Frame interpolation (frame_interp): 1 off, 2-4 frames shown for each the
 * title draws (hle_d3d8_interp.c). */
int  hle_frame_interp(void);
void hle_set_frame_interp(int n);

/* Borderless fullscreen (Alt+Enter on Windows). */
int  hle_fullscreen(void);
void hle_set_fullscreen(int on);

/* The desktop's in-game settings menu (F2, or Start+Back on a pad):
 * hle_settings_menu.c. Open or closed; the input hook below eats the pad
 * while it is open. */
int  hle_settings_menu_open(void);
void hle_settings_menu_toggle(void);
/* What to draw: the menu's lines, or NULL while it is closed. */
const char *hle_settings_menu_text(void);
/* Called with every sample of a port (src/hle/input_host.c), after the
 * person and any script: Start+Back on port 1 opens or closes the menu, and
 * while it is open (or the combination is still held) the sample is the
 * menu's and the game reads nothing. */
struct RecompInputGamepad;
void hle_settings_menu_input(unsigned port, struct RecompInputGamepad *g);
/* Keyboard navigation while open, from the window's own key reads:
 * dx/dy -1, 0, 1; ok and back as presses. */
void hle_settings_menu_keys(int dy, int dx, int ok, int back);

#ifdef __cplusplus
}
#endif

#endif
