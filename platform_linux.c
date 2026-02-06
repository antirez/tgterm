/*
 * platform_linux.c - Linux implementation using X11, XTest, and libpng.
 *
 * Requires: libx11-dev, libxtst-dev, libpng-dev
 */

#ifdef __linux__

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XTest.h>
#include <png.h>

static Display *dpy;

/* Known terminal WM_CLASS names on Linux. */
static const char *TerminalApps[] = {
    "gnome-terminal", "xterm", "kitty", "Alacritty", "alacritty",
    "ghostty", "Ghostty", "terminator", "tilix", "konsole",
    "xfce4-terminal", "mate-terminal", "lxterminal", "st", "stterm",
    "urxvt", "URxvt", "foot", "wezterm", "Wezterm",
    "hyper", "tabby", "sakura", "terminology", "guake", "tilda",
    NULL
};

void plat_init(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open X display. Is DISPLAY set?\n");
        exit(1);
    }
}

int plat_is_terminal(const char *name) {
    for (int i = 0; TerminalApps[i]; i++) {
        if (strcasestr(name, TerminalApps[i])) return 1;
    }
    return 0;
}

/* ---------- X11 helpers ---------- */

/* Get _NET_WM_PID property. Returns 0 if not set. */
static pid_t get_window_pid(Window win) {
    Atom prop = XInternAtom(dpy, "_NET_WM_PID", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, win, prop, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &data) != Success || !data)
        return 0;

    pid_t pid = 0;
    if (nitems > 0 && actual_format == 32)
        pid = (pid_t)(*(unsigned long *)data);
    XFree(data);
    return pid;
}

/* Get WM_CLASS. Returns the class name (second field) as malloc'd string. */
static char *get_wm_class(Window win) {
    XClassHint hint;
    memset(&hint, 0, sizeof(hint));
    if (!XGetClassHint(dpy, win, &hint)) return NULL;
    char *result = hint.res_class ? strdup(hint.res_class) : NULL;
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
    return result;
}

/* Get window title (_NET_WM_NAME or WM_NAME). Caller frees result. */
static char *get_window_title(Window win) {
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    /* Try _NET_WM_NAME first (UTF-8). */
    if (XGetWindowProperty(dpy, win, net_wm_name, 0, 1024, False, utf8,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &data) == Success && data && nitems) {
        char *title = strdup((char *)data);
        XFree(data);
        return title;
    }
    if (data) XFree(data);

    /* Fallback to WM_NAME. */
    data = NULL;
    if (XGetWindowProperty(dpy, win, XA_WM_NAME, 0, 1024, False, XA_STRING,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &data) == Success && data && nitems) {
        char *title = strdup((char *)data);
        XFree(data);
        return title;
    }
    if (data) XFree(data);
    return NULL;
}

/* Get _NET_CLIENT_LIST from the root window. Caller frees result. */
static Window *get_client_list(int *count) {
    Atom prop = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    Window root = DefaultRootWindow(dpy);
    if (XGetWindowProperty(dpy, root, prop, 0, 4096, False, XA_WINDOW,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &data) != Success || !data) {
        *count = 0;
        return NULL;
    }

    Window *list = malloc(nitems * sizeof(Window));
    if (!list) { XFree(data); *count = 0; return NULL; }
    memcpy(list, data, nitems * sizeof(Window));
    *count = (int)nitems;
    XFree(data);
    return list;
}

/* ---------- Window listing ---------- */

int plat_list_windows(WinInfo **out_list, int danger_mode) {
    *out_list = NULL;

    int wcount = 0;
    Window *clients = get_client_list(&wcount);
    if (!clients || wcount == 0) return 0;

    WinInfo *list = malloc(wcount * sizeof(WinInfo));
    if (!list) { free(clients); return 0; }
    int n = 0;

    for (int i = 0; i < wcount; i++) {
        char *wm_class = get_wm_class(clients[i]);
        if (!wm_class) continue;

        if (!danger_mode && !plat_is_terminal(wm_class)) {
            free(wm_class);
            continue;
        }

        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy, clients[i], &attr) ||
            attr.width <= 50 || attr.height <= 50) {
            free(wm_class);
            continue;
        }

        WinInfo *w = &list[n++];
        w->window_id = (PlatWinID)clients[i];
        w->pid = get_window_pid(clients[i]);

        strncpy(w->owner, wm_class, sizeof(w->owner) - 1);
        w->owner[sizeof(w->owner) - 1] = '\0';
        free(wm_class);

        char *title = get_window_title(clients[i]);
        if (title) {
            strncpy(w->title, title, sizeof(w->title) - 1);
            w->title[sizeof(w->title) - 1] = '\0';
            free(title);
        } else {
            w->title[0] = '\0';
        }
    }

    free(clients);
    *out_list = list;
    return n;
}

/* ---------- Window existence check ---------- */

int plat_window_exists(PlatWinID *wid, pid_t pid) {
    int wcount = 0;
    Window *clients = get_client_list(&wcount);
    if (!clients) return 0;

    int found = 0;
    PlatWinID fallback = 0;

    for (int i = 0; i < wcount; i++) {
        if ((PlatWinID)clients[i] == *wid) {
            found = 1;
            break;
        }
        if (!fallback && get_window_pid(clients[i]) == pid)
            fallback = (PlatWinID)clients[i];
    }

    if (!found && fallback) {
        *wid = fallback;
        found = 1;
    }

    free(clients);
    return found;
}

/* ---------- Screenshot ---------- */

/* Save XImage as PNG using libpng. */
static int save_ximage_png(XImage *img, const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              NULL, NULL, NULL);
    if (!png) { fclose(fp); return -1; }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, img->width, img->height, 8,
                 PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    unsigned char *row = malloc(3 * img->width);
    if (!row) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }

    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            unsigned long pixel = XGetPixel(img, x, y);
            row[x * 3]     = (pixel >> 16) & 0xFF;  /* R */
            row[x * 3 + 1] = (pixel >> 8) & 0xFF;   /* G */
            row[x * 3 + 2] = pixel & 0xFF;           /* B */
        }
        png_write_row(png, row);
    }

    free(row);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return 0;
}

int plat_capture_window(PlatWinID wid, const char *path) {
    Window win = (Window)wid;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, win, &attr)) return -1;

    /* Translate window origin to root coordinates so we can capture
     * from the root window. This is more reliable with compositing WMs
     * than capturing from the window drawable directly. */
    int rx, ry;
    Window child;
    XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy),
                          0, 0, &rx, &ry, &child);

    /* Clip to screen bounds. */
    int sw = DisplayWidth(dpy, DefaultScreen(dpy));
    int sh = DisplayHeight(dpy, DefaultScreen(dpy));
    int x = rx < 0 ? 0 : rx;
    int y = ry < 0 ? 0 : ry;
    int w = attr.width  - (x - rx);
    int h = attr.height - (y - ry);
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return -1;

    XImage *img = XGetImage(dpy, DefaultRootWindow(dpy),
                            x, y, w, h, AllPlanes, ZPixmap);
    if (!img) return -1;

    int ret = save_ximage_png(img, path);
    XDestroyImage(img);
    return ret;
}

/* ---------- Window focus ---------- */

void plat_raise_window(pid_t pid, PlatWinID wid) {
    (void)pid;
    Window win = (Window)wid;
    Window root = DefaultRootWindow(dpy);

    /* Send _NET_ACTIVE_WINDOW client message to the window manager. */
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 1;  /* Source: application */
    ev.xclient.data.l[1] = CurrentTime;
    ev.xclient.data.l[2] = 0;

    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XMapRaised(dpy, win);
    XFlush(dpy);
    usleep(100000);
}

/* ---------- Keystroke injection ---------- */

void plat_send_key(pid_t pid, int special, int ch, int mods) {
    (void)pid;  /* XTest sends to the focused window. */

    KeyCode keycode;
    int need_shift = 0;

    if (special == PLAT_KEY_RETURN) {
        keycode = XKeysymToKeycode(dpy, XK_Return);
    } else if (special == PLAT_KEY_TAB) {
        keycode = XKeysymToKeycode(dpy, XK_Tab);
    } else if (special == PLAT_KEY_ESCAPE) {
        keycode = XKeysymToKeycode(dpy, XK_Escape);
    } else if (special == PLAT_KEY_UP) {
        keycode = XKeysymToKeycode(dpy, XK_Up);
    } else if (special == PLAT_KEY_DOWN) {
        keycode = XKeysymToKeycode(dpy, XK_Down);
    } else if (special == PLAT_KEY_LEFT) {
        keycode = XKeysymToKeycode(dpy, XK_Left);
    } else if (special == PLAT_KEY_RIGHT) {
        keycode = XKeysymToKeycode(dpy, XK_Right);
    } else if (special == PLAT_KEY_PAGEUP) {
        keycode = XKeysymToKeycode(dpy, XK_Page_Up);
    } else if (special == PLAT_KEY_PAGEDN) {
        keycode = XKeysymToKeycode(dpy, XK_Page_Down);
    } else {
        /* Map ASCII character to keysym. X11 Latin-1 keysyms match ASCII. */
        KeySym sym = (KeySym)ch;
        keycode = XKeysymToKeycode(dpy, sym);
        if (!keycode) return;

        /* Determine if Shift is needed: compare with the unshifted keysym
         * at index 0 for this keycode. */
        KeySym base = XkbKeycodeToKeysym(dpy, keycode, 0, 0);
        if (base != sym) need_shift = 1;
    }

    if (!keycode) return;

    /* Press modifiers. */
    if (mods & MOD_CTRL)
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Control_L), True, 0);
    if (mods & MOD_ALT)
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Alt_L), True, 0);
    if (mods & MOD_CMD)
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Super_L), True, 0);
    if (need_shift)
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Shift_L), True, 0);

    /* Key press + release. */
    XTestFakeKeyEvent(dpy, keycode, True, 0);
    XTestFakeKeyEvent(dpy, keycode, False, 0);

    /* Release modifiers (reverse order). */
    if (need_shift)
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Shift_L), False, 0);
    if (mods & MOD_CMD)
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Super_L), False, 0);
    if (mods & MOD_ALT)
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Alt_L), False, 0);
    if (mods & MOD_CTRL)
        XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Control_L), False, 0);

    XFlush(dpy);
    usleep(5000);
}

#endif /* __linux__ */
