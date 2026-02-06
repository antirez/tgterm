/*
 * bot.c - Telegram bot to control terminal windows remotely.
 *
 * Works on macOS (Core Graphics) and Linux (X11 + XTest).
 *
 * Commands:
 *   .list    - List available terminal windows
 *   .1 .2 .. - Connect to window by number
 *   .help    - Show help
 *
 * Once connected, any text is sent as keystrokes (newline auto-added).
 * End with 💜 to suppress the automatic newline.
 * Emoji modifiers: ❤️ (Ctrl), 💙 (Alt), 💚 (Cmd/Super), 💛 (ESC)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>

#include "platform.h"
#include "botlib.h"
#include "sha1.h"
#include "qrcodegen.h"

/* ============================================================================
 * Global State
 * ========================================================================= */

static pthread_mutex_t RequestLock = PTHREAD_MUTEX_INITIALIZER;
static int DangerMode = 0;            /* If 1, show all windows, not just terminals. */
static WinInfo *WindowList = NULL;    /* Cached window list for .list display. */
static int WindowCount = 0;           /* Number of windows in list. */

/* TOTP authentication state. */
static int WeakSecurity = 0;          /* If 1, skip all OTP logic. */
static int Authenticated = 0;        /* Whether OTP has been verified. */
static time_t LastActivity = 0;      /* Last time owner sent a valid command. */
static int OtpTimeout = 300;         /* Timeout in seconds (default 5 min). */

/* Connected window. */
static int Connected = 0;
static PlatWinID ConnectedWid = 0;
static pid_t ConnectedPid = 0;
static char ConnectedOwner[128];
static char ConnectedTitle[256];

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

/* Print QR code as compact ASCII art using half-block characters. */
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
    int lo = -1, hi = size + 1;

    for (int y = lo; y < hi; y += 2) {
        for (int x = lo; x < hi; x++) {
            int top = (x >= 0 && x < size && y >= 0 && y < size &&
                       qrcodegen_getModule(qrcode, x, y));
            int bot = (x >= 0 && x < size && y+1 >= 0 && y+1 < size &&
                       qrcodegen_getModule(qrcode, x, y+1));
            if (top && bot)       printf("\xe2\x96\x88");
            else if (top && !bot) printf("\xe2\x96\x80");
            else if (!top && bot) printf("\xe2\x96\x84");
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

/* Setup TOTP: check for existing secret, generate if needed, display QR. */
static int totp_setup(const char *db_path) {
    if (WeakSecurity) return 0;

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database for TOTP setup.\n");
        return 0;
    }
    sqlite3_exec(db, TB_CREATE_KV_STORE, 0, 0, NULL);

    sds existing = kvGet(db, "totp_secret");
    if (existing) {
        sdsfree(existing);
        sds timeout_str = kvGet(db, "otp_timeout");
        if (timeout_str) {
            int t = atoi(timeout_str);
            if (t >= 30 && t <= 28800) OtpTimeout = t;
            sdsfree(timeout_str);
        }
        sqlite3_close(db);
        return 1;
    }

    unsigned char secret[20];
    FILE *f = fopen("/dev/urandom", "r");
    if (!f || fread(secret, 1, 20, f) != 20) {
        fprintf(stderr, "Failed to read /dev/urandom, aborting: "
                        "can't proceed without TOTP secret generation.\n");
        exit(1);
    }
    fclose(f);

    kvSet(db, "totp_secret", bytes_to_hex(secret, 20), 0);
    sqlite3_close(db);

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
        if (p[3] == 0x99) { *heart = 'B'; return 4; }
        if (p[3] == 0x9A) { *heart = 'G'; return 4; }
        if (p[3] == 0x9B) { *heart = 'Y'; return 4; }
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
 * Window Management (using platform interface)
 * ========================================================================= */

void free_window_list(void) {
    if (WindowList) {
        free(WindowList);
        WindowList = NULL;
    }
    WindowCount = 0;
}

int refresh_window_list(void) {
    free_window_list();
    WindowCount = plat_list_windows(&WindowList, DangerMode);
    return WindowCount;
}

int connected_window_exists(void) {
    if (!Connected) return 0;
    return plat_window_exists(&ConnectedWid, ConnectedPid);
}

void disconnect(void) {
    Connected = 0;
    ConnectedWid = 0;
    ConnectedPid = 0;
    ConnectedOwner[0] = '\0';
    ConnectedTitle[0] = '\0';
}

int capture_connected_window(const char *path) {
    if (!Connected) return -1;
    return plat_capture_window(ConnectedWid, path);
}

/* ============================================================================
 * Keystroke Sending
 * ========================================================================= */

/* Send keystrokes to connected window. Auto-adds newline unless ends with 💜. */
int send_keys(const char *text) {
    if (!Connected) return -1;

    plat_raise_window(ConnectedPid, ConnectedWid);

    int add_newline = !ends_with_purple_heart(text);

    const unsigned char *p = (const unsigned char *)text;
    size_t len = strlen(text);

    if (!add_newline && len >= 4)
        len -= 4;

    int mods = 0;
    int consumed;
    char heart;
    int keycount = 0;
    int had_mods = 0;
    int last_was_nl = 0;

    while (len > 0) {
        if ((consumed = match_red_heart(p, len)) > 0) {
            mods |= MOD_CTRL;
            p += consumed; len -= consumed;
            continue;
        }

        if ((consumed = match_orange_heart(p, len)) > 0) {
            plat_send_key(ConnectedPid, PLAT_KEY_RETURN, 0, mods);
            if (mods) had_mods = 1;
            keycount++; last_was_nl = 1; mods = 0;
            p += consumed; len -= consumed;
            continue;
        }

        if ((consumed = match_colored_heart(p, len, &heart)) > 0) {
            if (heart == 'Y') {
                plat_send_key(ConnectedPid, PLAT_KEY_ESCAPE, 0, 0);
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
                plat_send_key(ConnectedPid, PLAT_KEY_RETURN, 0, mods);
                if (mods) had_mods = 1;
                keycount++; last_was_nl = 1; mods = 0;
                p += 2; len -= 2;
                continue;
            } else if (p[1] == 't') {
                plat_send_key(ConnectedPid, PLAT_KEY_TAB, 0, mods);
                if (mods) had_mods = 1;
                keycount++; mods = 0; p += 2; len -= 2;
                continue;
            } else if (p[1] == '\\') {
                plat_send_key(ConnectedPid, PLAT_KEY_CHAR, '\\', mods);
                if (mods) had_mods = 1;
                keycount++; mods = 0; p += 2; len -= 2;
                continue;
            }
        }

        plat_send_key(ConnectedPid, PLAT_KEY_CHAR, (int)*p, mods);
        if (mods) had_mods = 1;
        keycount++; mods = 0;
        p++; len--;
    }

    if (add_newline && !(keycount == 1 && had_mods) && !last_was_nl) {
        usleep(50000);
        plat_send_key(ConnectedPid, PLAT_KEY_RETURN, 0, 0);
    }

    return 0;
}

/* ============================================================================
 * Bot Command Handlers
 * ========================================================================= */

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
            snprintf(line, sizeof(line), ".%d [%lu] %s - %s\n",
                     i + 1, w->window_id, w->owner, w->title);
        } else {
            snprintf(line, sizeof(line), ".%d [%lu] %s\n",
                     i + 1, w->window_id, w->owner);
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
        "Newline is auto-added; end with `\xf0\x9f\x92\x9c` to suppress it.\n\n"
        "Modifiers (tap to copy, then paste + key):\n"
        "`\xe2\x9d\xa4\xef\xb8\x8f` Ctrl  "
        "`\xf0\x9f\x92\x99` Alt  "
        "`\xf0\x9f\x92\x9a` Cmd/Super  "
        "`\xf0\x9f\x92\x9b` ESC  "
        "`\xf0\x9f\xa7\xa1` Enter\n\n"
        "Escape sequences: \\n=Enter \\t=Tab\n\n"
        "`.otptimeout <seconds>` - Set OTP timeout (30-28800)"
    );
}

/* ============================================================================
 * Telegram Bot Callbacks
 * ========================================================================= */

#define SCREENSHOT_PATH "/tmp/tgterm_screenshot.png"
#define OWNER_KEY "owner_id"
#define REFRESH_BTN "\xf0\x9f\x94\x84 Refresh"
#define REFRESH_DATA "refresh"

void send_screenshot(int64_t chat_id) {
    if (capture_connected_window(SCREENSHOT_PATH) != 0) return;
    botSendImageWithKeyboard(chat_id, SCREENSHOT_PATH, REFRESH_BTN, REFRESH_DATA, NULL);
}

void refresh_screenshot(int64_t chat_id, int64_t msg_id) {
    if (capture_connected_window(SCREENSHOT_PATH) != 0) return;
    botEditMessageMedia(chat_id, msg_id, SCREENSHOT_PATH, REFRESH_BTN, REFRESH_DATA);
}

void handle_request(sqlite3 *db, BotRequest *br) {
    pthread_mutex_lock(&RequestLock);

    sds owner_str = kvGet(db, OWNER_KEY);
    int64_t owner_id = 0;

    if (owner_str) {
        owner_id = strtoll(owner_str, NULL, 10);
        sdsfree(owner_str);
    }

    if (owner_id == 0) {
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

    if (!WeakSecurity) {
        if (!Authenticated || time(NULL) - LastActivity > OtpTimeout) {
            Authenticated = 0;
            if (br->is_callback) {
                botAnswerCallbackQuery(br->callback_id);
                goto done;
            }
            char *req = br->request;
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

    if (br->is_callback) {
        botAnswerCallbackQuery(br->callback_id);
        if (strcmp(br->callback_data, REFRESH_DATA) == 0 && Connected) {
            refresh_screenshot(br->target, br->msg_id);
        }
        goto done;
    }

    char *req = br->request;

    if (strcasecmp(req, ".list") == 0) {
        disconnect();
        sds msg = build_list_message();
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

    if (strcasecmp(req, ".help") == 0) {
        sds msg = build_help_message();
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

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

    if (req[0] == '.' && isdigit(req[1])) {
        int n = atoi(req + 1);
        refresh_window_list();

        if (n < 1 || n > WindowCount) {
            botSendMessage(br->target, "Invalid window number.", 0);
            goto done;
        }

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

        plat_raise_window(w->pid, w->window_id);
        send_screenshot(br->target);
        goto done;
    }

    if (!Connected) {
        sds msg = build_list_message();
        botSendMessage(br->target, msg, 0);
        sdsfree(msg);
        goto done;
    }

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

    send_keys(req);

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

    plat_init();
    totp_setup(dbfile);

    static char *triggers[] = { "*", NULL };

    startBot(TB_CREATE_KV_STORE, argc, argv, TB_FLAGS_IGNORE_BAD_ARG,
             handle_request, cron_callback, triggers);
    return 0;
}
