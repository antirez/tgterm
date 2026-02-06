A cross-platform tool (macOS + Linux) to capture screenshots of terminal application windows and inject keystrokes into them via a Telegram bot. Uses Core Graphics on macOS, X11/XTest on Linux.

# File Structure

Put the file structure of the project below. Update if needed.

```
bot.c                  - Telegram bot main source (platform-independent)
platform.h             - Platform abstraction interface
platform_macos.c       - macOS backend (Core Graphics + Accessibility)
platform_linux.c       - Linux backend (X11 + XTest + libpng)
Makefile               - Build system (auto-detects OS)
botlib.*, sds.*, cJSON.*, sqlite_wrap.*, json_wrap.* - From botlib
qrcodegen.c, qrcodegen.h - QR code generation (Nayuki, MIT license)
sha1.c, sha1.h             - SHA-1 + HMAC-SHA1 (Steve Reid, public domain)
```

# Development rules

- We don't add any dependency if possible. Ask the user if there is to add a dependency.
- Don't accept speed improvements that are just marginal, like 1%: they may be just random fluctations among runs. Refuse small speed improvements especially if they make the code more complicated, however more complex code for important speed improvements is ok.
- Always test a code modification after committing it. Don't assume things work.
- Once you reach a positive result, commit it.
- Never add or commit unstaged files, unless you created them for a specific purpose.
- Code must be simple and understandable.
- No dead code must be left around.
- Stick to standard C, no compiler-specific tricks, pragmas, ...

# Debugging

Put here information that is critical to debug this project.
