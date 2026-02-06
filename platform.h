/*
 * platform.h - Platform abstraction for window management, screenshots,
 *              and keystroke injection.
 *
 * Implementations: platform_macos.c (Core Graphics) and
 *                  platform_linux.c (X11 + XTest + libpng).
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <sys/types.h>

/* Platform-independent window ID. */
typedef unsigned long PlatWinID;

/* Modifier flags. */
#define MOD_CTRL    (1<<0)
#define MOD_ALT     (1<<1)
#define MOD_CMD     (1<<2)  /* Cmd on macOS, Super on Linux. */

/* Special key identifiers. */
#define PLAT_KEY_CHAR    0
#define PLAT_KEY_RETURN  1
#define PLAT_KEY_TAB     2
#define PLAT_KEY_ESCAPE  3

/* Window information. */
typedef struct {
    PlatWinID window_id;
    pid_t pid;
    char owner[128];
    char title[256];
} WinInfo;

/* Initialize platform resources. Call once at startup. */
void plat_init(void);

/* Check if app name is a known terminal. */
int plat_is_terminal(const char *name);

/* List terminal windows (or all windows if danger_mode).
 * Allocates *out_list; caller must free() it.
 * Returns the number of windows found. */
int plat_list_windows(WinInfo **out_list, int danger_mode);

/* Check if a window is still on screen.
 * If *wid is gone but the same PID has another on-screen window,
 * updates *wid to that window. Returns 1 if a window was found. */
int plat_window_exists(PlatWinID *wid, pid_t pid);

/* Capture window screenshot and save as PNG at path.
 * Returns 0 on success. */
int plat_capture_window(PlatWinID wid, const char *path);

/* Raise window to front and focus it. */
void plat_raise_window(pid_t pid, PlatWinID wid);

/* Send a single keystroke.
 * special: PLAT_KEY_CHAR to type 'ch', or PLAT_KEY_RETURN/TAB/ESCAPE.
 * ch: ASCII character (ignored unless special == PLAT_KEY_CHAR).
 * mods: bitmask of MOD_CTRL, MOD_ALT, MOD_CMD. */
void plat_send_key(pid_t pid, int special, int ch, int mods);

#endif
