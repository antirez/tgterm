/*
 * bot.c - Telegram bot to control terminal windows on macOS
 *
 * Allows capturing screenshots and sending keystrokes to terminal applications
 * (Terminal, iTerm2, Ghostty, kitty, etc.) via Telegram messages.
 *
 * Commands:
 *   .list    - List available terminal windows
 *   .1 .2 .. - Connect to window by number
 *   .help    - Show help
 *
 * Once connected, any text is sent as keystrokes (newline auto-added).
 * End with 💜 to suppress the automatic newline.
 * Emoji modifiers: ❤️ (Ctrl), 💙 (Alt), 💚 (Cmd), 💛 (ESC)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>
#include <ApplicationServices/ApplicationServices.h>

#include "botlib.h"
#include "sha1.h"
#include "qrcodegen.h"

/* ============================================================================
 * Terminal Window Management
 * ========================================================================= */

#define kVK_Return    0x24
#define kVK_Tab       0x30
#define kVK_Escape    0x35

#define MOD_CTRL    (1<<0)
#define MOD_ALT     (1<<1)
#define MOD_CMD     (1<<2)

/* Known terminal application names. */
static const char *TerminalApps[] = {
    "Terminal", "iTerm2", "iTerm", "Ghostty", "kitty", "Alacritty",
    "Hyper", "Warp", "WezTerm", "Tabby", NULL
};

/* Window information. */
typedef struct {
    CGWindowID window_id;
    pid_t pid;
    char owner[128];
    char title[256];
} WinInfo;

/* Global state. */
static pthread_mutex_t RequestLock = PTHREAD_MUTEX_INITIALIZER;
static int DangerMode = 0;            /* If 1, show all windows, not just terminals. */
static WinInfo *WindowList = NULL;    /* Cached window list for .list display. */
static int WindowCount = 0;           /* Number of windows in list. */

/* TOTP authentication state. */
static int WeakSecurity = 0;          /* If 1, skip all OTP logic. */
static int Authenticated = 0;        /* Whether OTP has been verified. */
static time_t LastActivity = 0;      /* Last time owner sent a valid command. */
static int OtpTimeout = 300;         /* Timeout in seconds (default 5 min). */

/* Connected window - stored directly, not as index. */
static int Connected = 0;             /* 1 if connected, 0 otherwise. */
static CGWindowID ConnectedWid = 0;   /* Window ID of connected window. */
static pid_t ConnectedPid = 0;        /* PID of connected window. */
static char ConnectedOwner[128];      /* Owner name for display. */
static char ConnectedTitle[256];      /* Title for display. */

/* ============================================================================
 * TOTP Authentication
 * ========================================================================= */

/* Encode raw bytes to Base32 string (RFC 4648). Returns static buffer. */
static const char *base32_encode(const unsigned char *data, size_t len) {
    static char out[128];
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    int i = 0, j = 0;
    uint64_t buf = 0;
    int bits = 0;

    for (i = 0; i < (int)len; i++) {
        buf = (buf << 8) | data[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out[j++] = alphabet[(buf >> bits) & 0x1f];
        }
    }
    if (bits > 0) {
        out[j++] = alphabet[(buf << (5 - bits)) & 0x1f];
    }
    out[j] = '\0';
    return out;
}

/* Compute 6-digit TOTP code from raw secret and time step. */
static uint32_t totp_code(const unsigned char *secret, size_t secret_len,
                          uint64_t time_step)
{
    unsigned char msg[8];
    for (int i = 7; i >= 0; i--) {
        msg[i] = (unsigned char)(time_step & 0xff);
        time_step >>= 8;
    }

    unsigned char hash[SHA1_DIGEST_SIZE];
    hmac_sha1(secret, secret_len, msg, 8, hash);

    int offset = hash[19] & 0x0f;
    uint32_t code = ((uint32_t)(hash[offset] & 0x7f) << 24)
                  | ((uint32_t)hash[offset+1] << 16)
                  | ((uint32_t)hash[offset+2] << 8)
                  | (uint32_t)hash[offset+3];
    return code % 1000000;
}

/* Print QR code as compact ASCII art using half-block characters.
 * Each output line encodes two QR rows using ▀ ▄ █ and space. */
static void print_qr_ascii(const char *text) {
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempbuf[qrcodegen_BUFFER_LEN_MAX];

    if (!qrcodegen_encodeText(text, tempbuf, qrcode,
            qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
            qrcodegen_Mask_AUTO, true)) {
        printf("Failed to generate QR code.\n");
        return;
    }

    int size = qrcodegen_getSize(qrcode);
    int lo = -1, hi = size + 1; /* 1-module quiet zone. */

    for (int y = lo; y < hi; y += 2) {
        for (int x = lo; x < hi; x++) {
            int top = (x >= 0 && x < size && y >= 0 && y < size &&
                       qrcodegen_getModule(qrcode, x, y));
            int bot = (x >= 0 && x < size && y+1 >= 0 && y+1 < size &&
                       qrcodegen_getModule(qrcode, x, y+1));
            if (top && bot)       printf("\xe2\x96\x88"); /* █ */
            else if (top && !bot) printf("\xe2\x96\x80"); /* ▀ */
            else if (!top && bot) printf("\xe2\x96\x84"); /* ▄ */
            else                  printf(" ");
        }
        printf("\n");
    }
}

/* Convert hex string to raw bytes. Returns number of bytes written. */
static int hex_to_bytes(const char *hex, unsigned char *out, int max) {
    int len = 0;
    while (*hex && *(hex+1) && len < max) {
        unsigned int byte;
        if (sscanf(hex, "%2x", &byte) != 1) break;
        out[len++] = (unsigned char)byte;
        hex += 2;
    }
    return len;
}

/* Convert raw bytes to hex string. Returns static buffer. */
static const char *bytes_to_hex(const unsigned char *data, int len) {
    static char hex[128];
    for (int i = 0; i < len && i < 63; i++) {
        sprintf(hex + i*2, "%02x", data[i]);
    }
    hex[len*2] = '\0';
    return hex;
}

/* Setup TOTP: check for existing secret, generate if needed, display QR.
 * The db_path is the SQLite database file path.
 * Returns the secret length in bytes, or 0 on error/weak-security. */
static int totp_setup(const char *db_path) {
    if (WeakSecurity) return 0;

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database for TOTP setup.\n");
        return 0;
    }
    /* Ensure KV table exists. */
    sqlite3_exec(db, TB_CREATE_KV_STORE, 0, 0, NULL);

    /* Check for existing secret. */
    sds existing = kvGet(db, "totp_secret");
    if (existing) {
        sdsfree(existing);
        /* Load stored timeout if present. */
        sds timeout_str = kvGet(db, "otp_timeout");
        if (timeout_str) {
            int t = atoi(timeout_str);
            if (t >= 30 && t <= 28800) OtpTimeout = t;
            sdsfree(timeout_str);
        }
        sqlite3_close(db);
        return 1; /* Secret already exists. */
    }

    /* Generate 20 random bytes. */
    unsigned char secret[20];
    FILE *f = fopen("/dev/urandom", "r");
    if (!f || fread(secret, 1, 20, f) != 20) {
        fprintf(stderr, "Failed to read /dev/urandom, aborting: "
                        "can't proceed without TOTP secret generation.\n");
        exit(1);
    }
    fclose(f);

    /* Store as hex in KV. */
    kvSet(db, "totp_secret", bytes_to_hex(secret, 20), 0);
    sqlite3_close(db);

    /* Build otpauth URI and display QR code. */
    const char *b32 = base32_encode(secret, 20);
    char uri[256];
    snprintf(uri, sizeof(uri),
             "otpauth://totp/tgterm?secret=%s&issuer=tgterm", b32);

    printf("\n=== TOTP Setup ===\n");
    printf("Scan this QR code with Google Authenticator:\n\n");
    print_qr_ascii(uri);
    printf("\nOr enter this secret manually: %s\n", b32);
    printf("==================\n\n");
    fflush(stdout);

    return 1;
}

/* Check if the given code matches the current TOTP (with ±1 window). */
static int totp_verify(sqlite3 *db, const char *code_str) {
    sds hex = kvGet(db, "totp_secret");
    if (!hex) return 0;

    unsigned char secret[20];
    int slen = hex_to_bytes(hex, secret, 20);
    sdsfree(hex);
    if (slen != 20) return 0;

    uint64_t now = (uint64_t)time(NULL) / 30;
    uint32_t input_code = (uint32_t)atoi(code_str);

    for (int i = -1; i <= 1; i++) {
        if (totp_code(secret, 20, now + i) == input_code)
            return 1;
    }
    return 0;
}

/* ============================================================================
 * UTF-8 Emoji Parsing
 * ========================================================================= */

/* Match red heart ❤️ (E2 9D A4, optionally followed by EF B8 8F). */
int match_red_heart(const unsigned char *p, size_t remaining) {
    if (remaining >= 3 && p[0] == 0xE2 && p[1] == 0x9D && p[2] == 0xA4) {
        if (remaining >= 6 && p[3] == 0xEF && p[4] == 0xB8 && p[5] == 0x8F)
            return 6;
        return 3;
    }
    return 0;
}

/* Match colored hearts 💙💚💛 (F0 9F 92 99/9A/9B). */
int match_colored_heart(const unsigned char *p, size_t remaining, char *heart) {
    if (remaining >= 4 && p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0x92) {
        if (p[3] == 0x99) { *heart = 'B'; return 4; }  /* 💙 Blue = Alt */
        if (p[3] == 0x9A) { *heart = 'G'; return 4; }  /* 💚 Green = Cmd */
        if (p[3] == 0x9B) { *heart = 'Y'; return 4; }  /* 💛 Yellow = ESC */
    }
    return 0;
}

/* Match orange heart 🧡 (F0 9F A7 A1) - sends Enter. */
int match_orange_heart(const unsigned char *p, size_t remaining) {
    if (remaining >= 4 && p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0xA7 && p[3] == 0xA1)
        return 4;
    return 0;
}

/* Match purple heart 💜 (F0 9F 92 9C) - used to suppress newline. */
int match_purple_heart(const unsigned char *p, size_t remaining) {
    if (remaining >= 4 && p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0x92 && p[3] == 0x9C)
        return 4;
    return 0;
}

/* Check if string ends with purple heart. */
int ends_with_purple_heart(const char *text) {
    size_t len = strlen(text);
    if (len >= 4) {
        const unsigned char *p = (const unsigned char *)text + len - 4;
        if (match_purple_heart(p, 4)) return 1;
    }
    return 0;
}

/* ============================================================================
 * Window Functions
 * ========================================================================= */

/* Check if app name is a known terminal. */
int is_terminal_app(const char *name) {
    for (int i = 0; TerminalApps[i]; i++) {
        if (strcasestr(name, TerminalApps[i])) return 1;
    }
    return 0;
}

/* Free the cached window list. */
void free_window_list(void) {
    if (WindowList) {
        free(WindowList);
        WindowList = NULL;
    }
    WindowCount = 0;
}

/* Refresh the window list. Returns number of windows found. */
int refresh_window_list(void) {
    free_window_list();

    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID
    );
    if (!list) return 0;

    CFIndex count = CFArrayGetCount(list);

    /* Allocate maximum possible size. */
    WindowList = malloc(count * sizeof(WinInfo));
    if (!WindowList) {
        CFRelease(list);
        return 0;
    }

    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = CFArrayGetValueAtIndex(list, i);

        /* Get owner name. */
        CFStringRef owner_ref = CFDictionaryGetValue(info, kCGWindowOwnerName);
        if (!owner_ref) continue;

        char owner[128];
        if (!CFStringGetCString(owner_ref, owner, sizeof(owner), kCFStringEncodingUTF8))
            continue;

        /* Filter to terminals only unless in danger mode. */
        if (!DangerMode && !is_terminal_app(owner)) continue;

        /* Get window ID and PID. */
        CFNumberRef wid_ref = CFDictionaryGetValue(info, kCGWindowNumber);
        CFNumberRef pid_ref = CFDictionaryGetValue(info, kCGWindowOwnerPID);
        if (!wid_ref || !pid_ref) continue;

        CGWindowID wid;
        pid_t pid;
        CFNumberGetValue(wid_ref, kCGWindowIDCFNumberType, &wid);
        CFNumberGetValue(pid_ref, kCFNumberIntType, &pid);

        /* Only layer 0. */
        CFNumberRef layer_ref = CFDictionaryGetValue(info, kCGWindowLayer);
        int layer = 0;
        if (layer_ref) CFNumberGetValue(layer_ref, kCFNumberIntType, &layer);
        if (layer != 0) continue;

        /* Must have reasonable size. */
        CFDictionaryRef bounds_dict = CFDictionaryGetValue(info, kCGWindowBounds);
        if (!bounds_dict) continue;

        CGRect bounds;
        CGRectMakeWithDictionaryRepresentation(bounds_dict, &bounds);
        if (bounds.size.width <= 50 || bounds.size.height <= 50) continue;

        /* Get window title. */
        CFStringRef title_ref = CFDictionaryGetValue(info, kCGWindowName);
        char title[256] = "";
        if (title_ref)
            CFStringGetCString(title_ref, title, sizeof(title), kCFStringEncodingUTF8);

        /* Add to list. */
        WinInfo *w = &WindowList[WindowCount++];
        w->window_id = wid;
        w->pid = pid;
        strncpy(w->owner, owner, sizeof(w->owner) - 1);
        w->owner[sizeof(w->owner) - 1] = '\0';
        strncpy(w->title, title, sizeof(w->title) - 1);
        w->title[sizeof(w->title) - 1] = '\0';
    }

    CFRelease(list);
    return WindowCount;
}

/* Check if connected window still exists on screen. If the exact window ID
 * is gone but the same PID still has an on-screen window (tab switch),
 * update ConnectedWid to the new window. */
int connected_window_exists(void) {
    if (!Connected) return 0;

    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID
    );
    if (!list) return 0;

    int found = 0;
    CGWindowID fallback_wid = 0;
    CFIndex count = CFArrayGetCount(list);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = CFArrayGetValueAtIndex(list, i);
        CFNumberRef wid_ref = CFDictionaryGetValue(info, kCGWindowNumber);
        CFNumberRef pid_ref = CFDictionaryGetValue(info, kCGWindowOwnerPID);
        if (!wid_ref || !pid_ref) continue;

        CGWindowID wid;
        pid_t pid;
        CFNumberGetValue(wid_ref, kCGWindowIDCFNumberType, &wid);
        CFNumberGetValue(pid_ref, kCFNumberIntType, &pid);

        if (wid == ConnectedWid) {
            found = 1;
            break;
        }

        /* Track a fallback: another on-screen window from the same PID. */
        if (pid == ConnectedPid && !fallback_wid) {
            CFNumberRef layer_ref = CFDictionaryGetValue(info, kCGWindowLayer);
            int layer = 0;
            if (layer_ref) CFNumberGetValue(layer_ref, kCFNumberIntType, &layer);
            if (layer == 0) fallback_wid = wid;
        }
    }

    /* Window gone but same app has another window — likely a tab switch. */
    if (!found && fallback_wid) {
        ConnectedWid = fallback_wid;
        found = 1;
    }

    CFRelease(list);
    return found;
}

/* Disconnect from current window. */
void disconnect(void) {
    Connected = 0;
    ConnectedWid = 0;
    ConnectedPid = 0;
    ConnectedOwner[0] = '\0';
    ConnectedTitle[0] = '\0';
}

/* ============================================================================
 * Screenshot Functions
 * ========================================================================= */

int save_png(CGImageRef image, const char *path) {
    CFStringRef cfpath = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, cfpath, kCFURLPOSIXPathStyle, false);
    CFRelease(cfpath);
    if (!url) return -1;

    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CFRelease(url);
    if (!dest) return -1;

    CGImageDestinationAddImage(dest, image, NULL);
    int ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    return ok ? 0 : -1;
}

CGImageRef capture_window(CGWindowID wid) {
    return CGWindowListCreateImage(CGRectNull, kCGWindowListOptionIncludingWindow, wid,
        kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);
}

/* Capture and save screenshot of connected window. Returns 0 on success. */
int capture_connected_window(const char *path) {
    if (!Connected) return -1;

    CGImageRef img = capture_window(ConnectedWid);
    if (!img) return -1;

    int ret = save_png(img, path);
    CGImageRelease(img);
    return ret;
}

/* ============================================================================
 * Keystroke Functions
 * ========================================================================= */

/* Private API to get CGWindowID from AXUIElement. */
extern AXError _AXUIElementGetWindow(AXUIElementRef element, CGWindowID *wid);

/* Bring app to front. */
int bring_to_front(pid_t pid) {
    ProcessSerialNumber psn;
    if (GetProcessForPID(pid, &psn) != noErr) return -1;
    if (SetFrontProcessWithOptions(&psn, kSetFrontProcessFrontWindowOnly) != noErr) return -1;
    usleep(100000);
    return 0;
}

/* Raise the specific window by matching CGWindowID via Accessibility API. */
int raise_window_by_id(pid_t pid, CGWindowID target_wid) {
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (!app) return -1;

    CFArrayRef windows = NULL;
    AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, (CFTypeRef *)&windows);
    CFRelease(app);

    if (!windows) return -1;

    int found = 0;
    CFIndex count = CFArrayGetCount(windows);
    for (CFIndex i = 0; i < count; i++) {
        AXUIElementRef win = (AXUIElementRef)CFArrayGetValueAtIndex(windows, i);

        CGWindowID wid = 0;
        if (_AXUIElementGetWindow(win, &wid) == kAXErrorSuccess) {
            if (wid == target_wid) {
                AXUIElementPerformAction(win, kAXRaiseAction);
                found = 1;
                break;
            }
        }
    }

    CFRelease(windows);

    /* Also bring the app to front. */
    bring_to_front(pid);
    return found ? 0 : -1;
}

void send_key(pid_t pid, CGKeyCode keycode, UniChar ch, int mods) {
    CGEventRef down = CGEventCreateKeyboardEvent(NULL, keycode, true);
    CGEventRef up = CGEventCreateKeyboardEvent(NULL, keycode, false);
    if (!down || !up) {
        if (down) CFRelease(down);
        if (up) CFRelease(up);
        return;
    }

    CGEventFlags flags = 0;
    if (mods & MOD_CTRL) flags |= kCGEventFlagMaskControl;
    if (mods & MOD_ALT)  flags |= kCGEventFlagMaskAlternate;
    if (mods & MOD_CMD)  flags |= kCGEventFlagMaskCommand;

    if (flags) {
        CGEventSetFlags(down, flags);
        CGEventSetFlags(up, flags);
    }

    if (ch) {
        CGEventKeyboardSetUnicodeString(down, 1, &ch);
        CGEventKeyboardSetUnicodeString(up, 1, &ch);
    }

    CGEventPostToPid(pid, down);
    usleep(1000);
    CGEventPostToPid(pid, up);
    usleep(5000);

    CFRelease(down);
    CFRelease(up);
}

/* Send keystrokes to connected window. Auto-adds newline unless ends with 💜. */
int send_keys(const char *text) {
    if (!Connected) return -1;

    raise_window_by_id(ConnectedPid, ConnectedWid);

    /* Check if we should suppress trailing newline. */
    int add_newline = !ends_with_purple_heart(text);

    const unsigned char *p = (const unsigned char *)text;
    size_t len = strlen(text);

    /* If ends with purple heart, reduce length to skip it. */
    if (!add_newline && len >= 4) {
        len -= 4;
    }

    int mods = 0;
    int consumed;
    char heart;
    int keycount = 0;       /* Number of actual keystrokes sent. */
    int had_mods = 0;       /* True if any keystroke used modifiers. */
    int last_was_nl = 0;    /* True if last keystroke was Enter. */

    while (len > 0) {
        if ((consumed = match_red_heart(p, len)) > 0) {
            mods |= MOD_CTRL;
            p += consumed; len -= consumed;
            continue;
        }

        if ((consumed = match_orange_heart(p, len)) > 0) {
            send_key(ConnectedPid, kVK_Return, 0, mods);
            if (mods) had_mods = 1;
            keycount++; last_was_nl = 1; mods = 0;
            p += consumed; len -= consumed;
            continue;
        }

        if ((consumed = match_colored_heart(p, len, &heart)) > 0) {
            if (heart == 'Y') {
                send_key(ConnectedPid, kVK_Escape, 0, 0);
                keycount++; had_mods = 1; last_was_nl = 0;
                mods = 0;
            } else if (heart == 'B') {
                mods |= MOD_ALT;
            } else if (heart == 'G') {
                mods |= MOD_CMD;
            }
            p += consumed; len -= consumed;
            continue;
        }

        last_was_nl = 0;
        if (*p == '\\' && len > 1) {
            if (p[1] == 'n') {
                send_key(ConnectedPid, kVK_Return, 0, mods);
                if (mods) had_mods = 1;
                keycount++; last_was_nl = 1; mods = 0;
                p += 2; len -= 2;
                continue;
            } else if (p[1] == 't') {
                send_key(ConnectedPid, kVK_Tab, 0, mods);
                if (mods) had_mods = 1;
                keycount++; mods = 0; p += 2; len -= 2;
                continue;
            } else if (p[1] == '\\') {
                send_key(ConnectedPid, 0, '\\', mods);
                if (mods) had_mods = 1;
                keycount++; mods = 0; p += 2; len -= 2;
                continue;
            }
        }

        send_key(ConnectedPid, 0, (UniChar)*p, mods);
        if (mods) had_mods = 1;
        keycount++; mods = 0;
        p++; len--;
    }

    /* Add newline unless:
     * - Suppressed by purple heart
     * - Single modified keystroke (like Ctrl+C) or bare ESC
     * - Last explicit keystroke was already a newline */
    if (add_newline && !(keycount == 1 && had_mods) && !last_was_nl) {
        usleep(50000);
        send_key(ConnectedPid, kVK_Return, 0, 0);
    }

    return 0;
}

/* ============================================================================
 * Bot Command Handlers
 * ========================================================================= */

/* Build the .list response. */
sds build_list_message(void) {
    refresh_window_list();

    sds msg = sdsempty();
    if (WindowCount == 0) {
        msg = sdscat(msg, "No terminal windows found.");
        return msg;
    }

    msg = sdscat(msg, "Terminal windows:\n");
    for (int i = 0; i < WindowCount; i++) {
        WinInfo *w = &WindowList[i];
        char line[512];
        if (w->title[0]) {
            snprintf(line, sizeof(line), ".%d [%u] %s - %s\n", i + 1, w->window_id, w->owner, w->title);
        } else {
            snprintf(line, sizeof(line), ".%d [%u] %s\n", i + 1, w->window_id, w->owner);
        }
        msg = sdscat(msg, line);
    }
    return msg;
}

sds build_help_message(void) {
    return sdsnew(
        "Commands:\n"
        ".list - Show terminal windows\n"
        ".1 .2 ... - Connect to window\n"
        ".help - This help\n\n"
        "Once connected, text is sent as keystrokes.\n"
        "Newline is auto-added; end with `💜` to suppress it.\n\n"
        "Modifiers (tap to copy, then paste + key):\n"
        "`❤️` Ctrl  `💙` Alt  `💚` Cmd  `💛` ESC  `🧡` Enter\n\n"
        "Escape sequences: \\n=Enter \\t=Tab\n\n"
        "`.otptimeout <seconds>` - Set OTP timeout (30-28800)"
    );
}

/* ============================================================================
 * Telegram Bot Callbacks
 * ========================================================================= */

#define SCREENSHOT_PATH "/tmp/tgterm_screenshot.png"
#define OWNER_KEY "owner_id"
#define REFRESH_BTN "🔄 Refresh"
#define REFRESH_DATA "refresh"

/* Send screenshot with refresh button. */
void send_screenshot(int64_t chat_id) {
    if (capture_connected_window(SCREENSHOT_PATH) != 0) return;
    botSendImageWithKeyboard(chat_id, SCREENSHOT_PATH, REFRESH_BTN, REFRESH_DATA, NULL);
}

/* Refresh an existing screenshot message by editing its media. */
void refresh_screenshot(int64_t chat_id, int64_t msg_id) {
    if (capture_connected_window(SCREENSHOT_PATH) != 0) return;
    botEditMessageMedia(chat_id, msg_id, SCREENSHOT_PATH, REFRESH_BTN, REFRESH_DATA);
}

void handle_request(sqlite3 *db, BotRequest *br) {
    pthread_mutex_lock(&RequestLock);

    /* Check owner. First user to message becomes owner. */
    sds owner_str = kvGet(db, OWNER_KEY);
    int64_t owner_id = 0;

    if (owner_str) {
        owner_id = strtoll(owner_str, NULL, 10);
        sdsfree(owner_str);
    }

    if (owner_id == 0) {
        /* Register first user as owner. */
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)br->from);
        kvSet(db, OWNER_KEY, buf, 0);
        owner_id = br->from;
        printf("Registered owner: %lld (%s)\n", (long long)owner_id, br->from_username);
    }

    if (br->from != owner_id) {
        printf("Ignoring message from non-owner %lld\n", (long long)br->from);
        goto done;
    }

    /* TOTP authentication check (applies to both messages and callbacks). */
    if (!WeakSecurity) {
        if (!Authenticated || time(NULL) - LastActivity > OtpTimeout) {
            Authenticated = 0;
            if (br->is_callback) {
                botAnswerCallbackQuery(br->callback_id);
                goto done;
            }
            char *req = br->request;
            /* Check if message is a 6-digit OTP code. */
            int is_otp = (strlen(req) == 6);
            for (int i = 0; is_otp && i < 6; i++) {
                if (!isdigit((unsigned char)req[i])) is_otp = 0;
            }
            if (is_otp && totp_verify(db, req)) {
                Authenticated = 1;
                LastActivity = time(NULL);
                botSendMessage(br->target, "Authenticated.", 0);
            } else {
                botSendMessage(br->target, "Enter OTP code.", 0);
            }
            goto done;
        }
        LastActivity = time(NULL);
    }

    /* Handle callback query (button press). */
    if (br->is_callback) {
        botAnswerCallbackQuery(br->callback_id);
        if (strcmp(br->callback_data, REFRESH_DATA) == 0 && Connected) {
            refresh_screenshot(br->target, br->msg_id);
        }
        goto done;
    }

    char *req = br->request;

    /* Handle .list command. */
    if (strcasecmp(req, ".list") == 0) {
        disconnect();
        sds msg = build_list_message();
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    /* Handle .help command. */
    if (strcasecmp(req, ".help") == 0) {
        sds msg = build_help_message();
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    /* Handle .otptimeout command. */
    if (strncasecmp(req, ".otptimeout", 11) == 0) {
        char *arg = req + 11;
        while (*arg == ' ') arg++;
        int secs = atoi(arg);
        if (secs < 30) secs = 30;
        if (secs > 28800) secs = 28800;
        OtpTimeout = secs;
        char buf[64];
        snprintf(buf, sizeof(buf), "%d", secs);
        kvSet(db, "otp_timeout", buf, 0);
        sds msg = sdscatprintf(sdsempty(), "OTP timeout set to %d seconds.", secs);
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    /* Handle .N to connect to window N. */
    if (req[0] == '.' && isdigit(req[1])) {
        int n = atoi(req + 1);
        refresh_window_list();

        if (n < 1 || n > WindowCount) {
            botSendMessage(br->target, "Invalid window number.", 0);
            goto done;
        }

        /* Store connection info directly. */
        WinInfo *w = &WindowList[n - 1];
        Connected = 1;
        ConnectedWid = w->window_id;
        ConnectedPid = w->pid;
        strncpy(ConnectedOwner, w->owner, sizeof(ConnectedOwner) - 1);
        ConnectedOwner[sizeof(ConnectedOwner) - 1] = '\0';
        strncpy(ConnectedTitle, w->title, sizeof(ConnectedTitle) - 1);
        ConnectedTitle[sizeof(ConnectedTitle) - 1] = '\0';

        sds msg = sdsnew("Connected to ");
        msg = sdscat(msg, ConnectedOwner);
        if (ConnectedTitle[0]) {
            msg = sdscat(msg, " - ");
            msg = sdscat(msg, ConnectedTitle);
        }
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);

        /* Raise the window and send welcome screenshot. */
        raise_window_by_id(w->pid, w->window_id);
        send_screenshot(br->target);
        goto done;
    }

    /* Not a command - send as keystrokes if connected. */
    if (!Connected) {
        sds msg = build_list_message();
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    /* Check window still exists. */
    if (!connected_window_exists()) {
        disconnect();
        sds msg = sdsnew("Window closed.\n\n");
        sds list = build_list_message();
        msg = sdscatsds(msg, list);
        sdsfree(list);
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    /* Send keystrokes. */
    send_keys(req);

    /* Wait a bit for the terminal to react, then re-check the window
     * (keystrokes like ESC+N may switch tabs, changing the window ID). */
    sleep(2);
    connected_window_exists();
    send_screenshot(br->target);

done:
    pthread_mutex_unlock(&RequestLock);
}

void cron_callback(sqlite3 *db) {
    UNUSED(db);
}

/* ============================================================================
 * Main
 * ========================================================================= */

int main(int argc, char **argv) {
    /* Parse our custom flags. */
    const char *dbfile = "./mybot.sqlite";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dangerously-attach-to-any-window") == 0) {
            DangerMode = 1;
            printf("DANGER MODE: All windows will be visible.\n");
        } else if (strcmp(argv[i], "--use-weak-security") == 0) {
            WeakSecurity = 1;
            printf("WARNING: OTP authentication disabled.\n");
        } else if (strcmp(argv[i], "--dbfile") == 0 && i+1 < argc) {
            dbfile = argv[i+1];
        }
    }

    /* TOTP setup: check/generate secret before starting the bot. */
    totp_setup(dbfile);

    /* Triggers: respond to all private messages. */
    static char *triggers[] = { "*", NULL };

    startBot(TB_CREATE_KV_STORE, argc, argv, TB_FLAGS_IGNORE_BAD_ARG,
             handle_request, cron_callback, triggers);
    return 0;
}
