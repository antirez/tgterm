# tgterm

## Motivations

Since coding agents started reshaping the way we write code, many developers started to feel the need to access their terminals while away from the keyboard. Providing instructions to agents, for them to continue the development, is now possible without typing much, just observing, evaluating, and sending further instructions: something that can be done via a phone keyboard.

The usual setup now is to: setup some kind of SSH tunneling, or VPN between the computer and the phone. Starting agent coding sessions via Tmux, and then using Termux or a plain mobile SSH client in order to access remote sessions. But, I consider Telegram a better way to access my sessions for the following reasons:

1. Terminal is already on your phone, and you get actual screenshots of your terminals, that you can update with a click on a button. You can just type in just writing a Telegram message, that is what you need to work with Claude Code, Codex and other agents.
2. I don't need to remember to start sessions multiplexed with tmux / screen. For instance, I don't use multiplexing normally, why caring?
3. Setup burden of SSH tunnels, VPNs, ...
4. SSH does not work in a very important case: if the development at hand involves inspecting graphical output of any kind. This is extremely important in certain setups: for instance, in the development of [flux2.c](https://github.com/antirez/flux2.c), for me it makes a lot of a difference.

So I built this project, that allows to control many terminal sessions using Telegram.

![tgterm in action](tgterm.png)

This is how it works:

1. You talk with a Telegram bot, that you create only for yourself.
2. After you setup your TOTP, you send the bot the first message, and you become its owner. It will only accept queries from you (your Telegram ID) and will require you to authenticat with an OTP for the first time, and again after a timeout.
3. At this point, you can ask for the list of terminal windows in your system with `.list`, connect to one of them with (for instance) `.2`, then you can send any text that will be "typed" in the window, like if you are still at your computer. You have modifiers, ways to send `ESC`, and so forth, so you can do many things, like changing the visible tab.

**Supported platforms:** macOS and Linux (X11).

## First run

### macOS permissions

The program requires two system permissions on macOS:

- **Screen Recording** — needed to capture terminal window screenshots.
- **Accessibility** — needed to inject keystrokes and raise windows.

macOS will prompt you to grant these on first use. If screenshots or keystrokes silently fail, check System Settings → Privacy & Security.

### Setup

1. Create a Telegram bot via [@BotFather](https://t.me/botfather) and get the API key.
2. Install dependencies:
   - **macOS:** `libcurl` and `libsqlite3` (usually pre-installed).
   - **Linux:** `sudo apt install libx11-dev libxtst-dev libpng-dev libcurl4-openssl-dev libsqlite3-dev`
3. Build with `make` and run:

```
./tgterm --apikey <your-api-key>
```

4. On the first run, the bot will display a QR code and a text secret on your terminal:

```
=== TOTP Setup ===
Scan this QR code with Google Authenticator:

 ▄▄▄▄▄▄▄  ▄▄ ▄▄▄   ▄    ▄  ▄▄▄▄▄▄▄
 █ ▄▄▄ █ █▄▄█▀▀ █▀▄▄▄▄▀▄█▀ █ ▄▄▄ █
 ...

Or enter this secret manually: KAPLXWZSYXOR64Y49HG75IIFXHSQLHTY
==================
```

Open Google Authenticator (or any TOTP app), scan the QR code or type the secret manually. This is the only time the secret is shown — you won't see it again on subsequent runs.

4. Send any message to your bot on Telegram (e.g. `/start` or just `hello`). The first user to message becomes the **owner**. The bot will only accept messages from you from now on.

5. The bot will reply asking for your OTP code. Type the 6-digit code from your authenticator app to unlock it.

You're ready. Type `.list` to see your terminal windows and `.help` for the full command reference.

## Usage

### Commands

- `.list` — List available terminal windows.
- `.1`, `.2`, ... — Connect to a window by its number.
- `.help` — Show the help message.
- `.otptimeout <seconds>` — Set the OTP inactivity timeout (range: 30–28800 seconds). Default is 300 (5 minutes).

### Sending keystrokes

Once connected to a window, any text you send is typed into it as keystrokes. A newline (Enter) is automatically appended at the end.

**Suppressing the newline:** End your message with 💜 to prevent the automatic Enter at the end. This is useful for partially typing a command or entering text without submitting it.

**Modifier emojis:** The `.help` message includes modifier emojis formatted in backticks **so that you can tap to copy them on your phone**. Paste a modifier before a character to send that key combination:

- ❤️ — Ctrl (e.g. `❤️c` sends Ctrl+C)
- 💙 — Alt
- 💚 — Cmd
- 💛 — ESC (sends Escape immediately, no following key needed)
- 🧡 — Enter (sends Enter at that position, useful for multi-line input)

Modifiers can be combined: `❤️💙x` sends Ctrl+Alt+X. A single modified keystroke (like `❤️c`) will not have an automatic newline appended.

**Escape sequences:** `\n` sends Enter, `\t` sends Tab, `\\` sends a literal backslash.

### Screenshots

After every keystroke message, the bot waits briefly for the terminal to update and then sends back a screenshot of the connected window. The screenshot includes a 🔄 Refresh button that you can tap to get an updated screenshot without sending any keystrokes.

## Security

This tool allows remote control of terminal windows via Telegram. Given the sensitivity of this capability, multiple layers of security are in place:

**Ownership.** The first Telegram user to message the bot becomes its owner. All subsequent messages from other users are silently ignored. The owner's user ID is stored persistently in the database.

**TOTP authentication.** On first startup, the bot generates a TOTP shared secret and displays a QR code on the terminal for scanning with Google Authenticator (or any TOTP app). After a configurable period of inactivity (default 5 minutes), the bot locks and requires the owner to enter a valid one-time password before accepting further commands. This protects against scenarios where an attacker gains access to the owner's Telegram account. The timeout is configurable via `.otptimeout`.

**Terminal-only window access.** By default, the bot only lists and connects to known terminal applications (Terminal, iTerm2, Ghostty, kitty, Alacritty, etc.). This limits the attack surface compared to allowing control of arbitrary windows. Note however that this is a convenience restriction, not a real security boundary: anyone with control of a terminal can trivially escape to full system access.

**Resetting authentication.** To reset everything (owner, TOTP secret, all settings), delete the `mybot.sqlite` file and restart the bot. A new TOTP secret will be generated and a new QR code displayed.

**Disabling TOTP.** If you don't want OTP authentication (not recommended), run with `--use-weak-security`. The bot will still enforce ownership but will not require OTP codes.

## Headless / VM usage

tgterm can run on headless servers and VMs (no physical monitor) using Xvfb, a virtual X framebuffer. This is useful for controlling coding agents remotely from your phone.

### Quick setup

Install the required packages:

```
sudo apt install xvfb openbox xterm
```

Start the virtual display, a window manager, and a terminal:

```
Xvfb :99 -screen 0 1920x1080x24 &
DISPLAY=:99 openbox &
DISPLAY=:99 xterm -fa Monospace -fs 14 -geometry 120x40 &
```

Then run tgterm pointed at the virtual display:

```
DISPLAY=:99 ./tgterm --apikey <your-api-key>
```

You can launch as many xterm sessions as you need and switch between them with `.list` and `.1`, `.2`, etc. from Telegram.

### Resolution

Change the Xvfb screen size for higher resolution screenshots. For example, 2560x1440:

```
Xvfb :99 -screen 0 2560x1440x24 &
```

### Terminal theme and colors

xterm reads `~/.Xresources` for appearance settings. Load them with `xrdb` **before** starting xterm:

```
DISPLAY=:99 xrdb -merge ~/.Xresources
DISPLAY=:99 xterm -title "Session 1" &
```

Example `~/.Xresources` with a dark theme (Catppuccin Mocha):

```
xterm*faceName: DejaVu Sans Mono
xterm*faceSize: 13
xterm*renderFont: true
xterm*termName: xterm-256color
xterm*scrollBar: false
xterm*saveLines: 4096
xterm*geometry: 140x45

xterm*background: #1e1e2e
xterm*foreground: #cdd6f4
xterm*cursorColor: #f5e0dc

xterm*color0: #45475a
xterm*color1: #f38ba8
xterm*color2: #a6e3a1
xterm*color3: #f9e2af
xterm*color4: #89b4fa
xterm*color5: #f5c2e7
xterm*color6: #94e2d5
xterm*color7: #bac2de
xterm*color8: #585b70
xterm*color9: #f38ba8
xterm*color10: #a6e3a1
xterm*color11: #f9e2af
xterm*color12: #89b4fa
xterm*color13: #f5c2e7
xterm*color14: #94e2d5
xterm*color15: #f5e0dc
```

You'll also need `x11-xserver-utils` installed for `xrdb`:

```
sudo apt install x11-xserver-utils
```

### What you need

- **Xvfb** — virtual X server (renders to memory, no GPU needed).
- **A window manager** — openbox is lightweight and sufficient. Required so that tgterm can enumerate windows via `_NET_CLIENT_LIST`.
- **A terminal emulator** — xterm works everywhere. Any X11 terminal will do.

## Limitations

- **Deprecated macOS APIs.** The project uses older Core Graphics and Process Manager APIs for screenshot capture and window management. These produce compiler warnings on macOS 14+ but still work correctly, and provide good compatibility with older macOS versions. They will be replaced if and when Apple removes them.
- **Linux: X11 only.** The Linux backend requires an X11 display server. Wayland is not supported (XWayland may work).
- **Linux: screenshots require visible windows.** On Linux, the screenshot is captured from the root window at the window's position, so the window must be visible (not occluded). The bot raises the window before capturing, which handles this in most cases.
- **UTF-8 keystrokes.** Non-ASCII text (beyond the special emoji modifiers) is not handled correctly when sending keystrokes. Only ASCII characters are reliably injected.

## Credits

* **QR Code generator library** by [Project Nayuki](https://www.nayuki.io/page/qr-code-generator-library) — MIT license.
* **SHA-1 implementation** by Steve Reid — 100% public domain.
