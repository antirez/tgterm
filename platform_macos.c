/*
 * platform_macos.c - macOS implementation using Core Graphics and Accessibility.
 */

#ifdef __APPLE__

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>
#include <ApplicationServices/ApplicationServices.h>

/* Known terminal application names on macOS. */
static const char *TerminalApps[] = {
    "Terminal", "iTerm2", "iTerm", "Ghostty", "kitty", "Alacritty",
    "Hyper", "Warp", "WezTerm", "Tabby", NULL
};

void plat_init(void) {
    /* Nothing needed on macOS. */
}

int plat_is_terminal(const char *name) {
    for (int i = 0; TerminalApps[i]; i++) {
        if (strcasestr(name, TerminalApps[i])) return 1;
    }
    return 0;
}

int plat_list_windows(WinInfo **out_list, int danger_mode) {
    *out_list = NULL;

    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID
    );
    if (!list) return 0;

    CFIndex count = CFArrayGetCount(list);
    WinInfo *wlist = malloc(count * sizeof(WinInfo));
    if (!wlist) { CFRelease(list); return 0; }

    int n = 0;
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = CFArrayGetValueAtIndex(list, i);

        CFStringRef owner_ref = CFDictionaryGetValue(info, kCGWindowOwnerName);
        if (!owner_ref) continue;

        char owner[128];
        if (!CFStringGetCString(owner_ref, owner, sizeof(owner),
                                kCFStringEncodingUTF8))
            continue;

        if (!danger_mode && !plat_is_terminal(owner)) continue;

        CFNumberRef wid_ref = CFDictionaryGetValue(info, kCGWindowNumber);
        CFNumberRef pid_ref = CFDictionaryGetValue(info, kCGWindowOwnerPID);
        if (!wid_ref || !pid_ref) continue;

        CGWindowID wid;
        pid_t pid;
        CFNumberGetValue(wid_ref, kCGWindowIDCFNumberType, &wid);
        CFNumberGetValue(pid_ref, kCFNumberIntType, &pid);

        CFNumberRef layer_ref = CFDictionaryGetValue(info, kCGWindowLayer);
        int layer = 0;
        if (layer_ref) CFNumberGetValue(layer_ref, kCFNumberIntType, &layer);
        if (layer != 0) continue;

        CFDictionaryRef bounds_dict = CFDictionaryGetValue(info, kCGWindowBounds);
        if (!bounds_dict) continue;
        CGRect bounds;
        CGRectMakeWithDictionaryRepresentation(bounds_dict, &bounds);
        if (bounds.size.width <= 50 || bounds.size.height <= 50) continue;

        CFStringRef title_ref = CFDictionaryGetValue(info, kCGWindowName);
        char title[256] = "";
        if (title_ref)
            CFStringGetCString(title_ref, title, sizeof(title),
                               kCFStringEncodingUTF8);

        WinInfo *w = &wlist[n++];
        w->window_id = (PlatWinID)wid;
        w->pid = pid;
        strncpy(w->owner, owner, sizeof(w->owner) - 1);
        w->owner[sizeof(w->owner) - 1] = '\0';
        strncpy(w->title, title, sizeof(w->title) - 1);
        w->title[sizeof(w->title) - 1] = '\0';
    }

    CFRelease(list);
    *out_list = wlist;
    return n;
}

int plat_window_exists(PlatWinID *wid, pid_t pid) {
    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID
    );
    if (!list) return 0;

    int found = 0;
    PlatWinID fallback = 0;
    CFIndex count = CFArrayGetCount(list);

    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef info = CFArrayGetValueAtIndex(list, i);
        CFNumberRef wid_ref = CFDictionaryGetValue(info, kCGWindowNumber);
        CFNumberRef pid_ref = CFDictionaryGetValue(info, kCGWindowOwnerPID);
        if (!wid_ref || !pid_ref) continue;

        CGWindowID cg_wid;
        pid_t cg_pid;
        CFNumberGetValue(wid_ref, kCGWindowIDCFNumberType, &cg_wid);
        CFNumberGetValue(pid_ref, kCFNumberIntType, &cg_pid);

        if ((PlatWinID)cg_wid == *wid) {
            found = 1;
            break;
        }

        if (cg_pid == pid && !fallback) {
            CFNumberRef layer_ref = CFDictionaryGetValue(info, kCGWindowLayer);
            int layer = 0;
            if (layer_ref)
                CFNumberGetValue(layer_ref, kCFNumberIntType, &layer);
            if (layer == 0) fallback = (PlatWinID)cg_wid;
        }
    }

    if (!found && fallback) {
        *wid = fallback;
        found = 1;
    }

    CFRelease(list);
    return found;
}

static int save_png(CGImageRef image, const char *path) {
    CFStringRef cfpath = CFStringCreateWithCString(NULL, path,
                                                   kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, cfpath,
                                                 kCFURLPOSIXPathStyle, false);
    CFRelease(cfpath);
    if (!url) return -1;

    CGImageDestinationRef dest =
        CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CFRelease(url);
    if (!dest) return -1;

    CGImageDestinationAddImage(dest, image, NULL);
    int ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    return ok ? 0 : -1;
}

int plat_capture_window(PlatWinID wid, const char *path) {
    CGImageRef img = CGWindowListCreateImage(
        CGRectNull, kCGWindowListOptionIncludingWindow, (CGWindowID)wid,
        kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);
    if (!img) return -1;

    int ret = save_png(img, path);
    CGImageRelease(img);
    return ret;
}

/* Private API to get CGWindowID from AXUIElement. */
extern AXError _AXUIElementGetWindow(AXUIElementRef element, CGWindowID *wid);

static int bring_to_front(pid_t pid) {
    ProcessSerialNumber psn;
    if (GetProcessForPID(pid, &psn) != noErr) return -1;
    if (SetFrontProcessWithOptions(&psn, kSetFrontProcessFrontWindowOnly)
        != noErr) return -1;
    usleep(100000);
    return 0;
}

void plat_raise_window(pid_t pid, PlatWinID wid) {
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (!app) { bring_to_front(pid); return; }

    CFArrayRef windows = NULL;
    AXUIElementCopyAttributeValue(app, kAXWindowsAttribute,
                                  (CFTypeRef *)&windows);
    CFRelease(app);

    if (windows) {
        CFIndex count = CFArrayGetCount(windows);
        for (CFIndex i = 0; i < count; i++) {
            AXUIElementRef win =
                (AXUIElementRef)CFArrayGetValueAtIndex(windows, i);
            CGWindowID cg_wid = 0;
            if (_AXUIElementGetWindow(win, &cg_wid) == kAXErrorSuccess) {
                if ((PlatWinID)cg_wid == wid) {
                    AXUIElementPerformAction(win, kAXRaiseAction);
                    break;
                }
            }
        }
        CFRelease(windows);
    }

    bring_to_front(pid);
}

/* Map ASCII character to macOS virtual keycode (US keyboard layout). */
static CGKeyCode keycode_for_char(char c) {
    static const CGKeyCode letter_map[26] = {
        0x00,0x0B,0x08,0x02,0x0E,0x03,0x05,0x04,0x22,0x26,
        0x28,0x25,0x2E,0x2D,0x1F,0x23,0x0C,0x0F,0x01,0x11,
        0x20,0x09,0x0D,0x07,0x10,0x06
    };
    static const CGKeyCode digit_map[10] = {
        0x1D,0x12,0x13,0x14,0x15,0x17,0x16,0x1A,0x1C,0x19
    };
    if (c >= 'a' && c <= 'z') return letter_map[c - 'a'];
    if (c >= 'A' && c <= 'Z') return letter_map[c - 'A'];
    if (c >= '0' && c <= '9') return digit_map[c - '0'];
    switch (c) {
        case '-':  return 0x1B;  case '=':  return 0x18;
        case '[':  return 0x21;  case ']':  return 0x1E;
        case '\\': return 0x2A;  case ';':  return 0x29;
        case '\'': return 0x27;  case ',':  return 0x2B;
        case '.':  return 0x2F;  case '/':  return 0x2C;
        case '`':  return 0x32;  case ' ':  return 0x31;
    }
    return 0xFFFF;
}

void plat_send_key(pid_t pid, int special, int ch, int mods) {
    CGKeyCode keycode;
    UniChar uc = 0;
    int mapped_keycode = 0;

    switch (special) {
    case PLAT_KEY_RETURN:  keycode = 0x24; break;
    case PLAT_KEY_TAB:     keycode = 0x30; break;
    case PLAT_KEY_ESCAPE:  keycode = 0x35; break;
    case PLAT_KEY_UP:      keycode = 0x7E; break;
    case PLAT_KEY_DOWN:    keycode = 0x7D; break;
    case PLAT_KEY_LEFT:    keycode = 0x7B; break;
    case PLAT_KEY_RIGHT:   keycode = 0x7C; break;
    case PLAT_KEY_PAGEUP:  keycode = 0x74; break;
    case PLAT_KEY_PAGEDN:  keycode = 0x79; break;
    default:
        uc = (UniChar)ch;
        if (mods) {
            CGKeyCode mapped = keycode_for_char((char)ch);
            if (mapped != 0xFFFF) {
                keycode = mapped;
                mapped_keycode = 1;
            } else {
                keycode = 0;
            }
        } else {
            keycode = 0;
        }
        break;
    }

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

    if (uc && !mapped_keycode) {
        CGEventKeyboardSetUnicodeString(down, 1, &uc);
        CGEventKeyboardSetUnicodeString(up, 1, &uc);
    }

    CGEventPostToPid(pid, down);
    usleep(1000);
    CGEventPostToPid(pid, up);
    usleep(5000);

    CFRelease(down);
    CFRelease(up);
}

#endif /* __APPLE__ */
