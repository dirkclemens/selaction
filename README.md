# codexpopclip

Small Qt6 daemon-like app for KDE Plasma (Wayland) that watches clipboard/selection changes and shows a popup menu with text actions.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/codexpopclip
```

## Autostart (KDE Plasma)

Create `~/.config/autostart/codexpopclip.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=codexpopclip
Exec=/home/user/Projekte/cpp/codexpopclip/build/codexpopclip
X-KDE-autostart-after=panel
```

## Wayland notes

- Wayland does not allow global text selection access like X11.
- Clipboard changes (Ctrl+C) are usually visible.
- Primary selection (mouse select) may work only on some compositors or apps.
- If an app does not export selection, the menu will not appear.

For debugging you can enable polling (every 500ms) to detect changes even if
signals are missing:

```bash
CODEXPOPCLIP_POLL=1 ./build/codexpopclip
```

If Qt cannot read clipboard contents under Wayland, you can enable a fallback
using `wl-paste` (requires `wl-clipboard`):

```bash
CODEXPOPCLIP_POLL=1 CODEXPOPCLIP_WLPASTE=1 ./build/codexpopclip
```

To reduce polling side effects (e.g. flicker), you can slow down polling and
limit wl-paste to primary selection only:

```bash
CODEXPOPCLIP_POLL=1 CODEXPOPCLIP_POLL_MS=1500 CODEXPOPCLIP_WLPASTE=1 \
  CODEXPOPCLIP_WLPASTE_MODE=primary ./build/codexpopclip
```

`CODEXPOPCLIP_WLPASTE_MODE` accepts: `primary`, `clipboard`, `both`.

## Settings config

Create `~/.config/codexpopclip/settings.json`:

```json
{
  "poll": true,
  "poll_ms": 1500,
  "wlpaste": true,
  "wlpaste_mode": "primary"
}
```

Environment variables still work and override the file when set.

## External actions config

Create the config file:

`~/.config/codexpopclip/actions.json`

Example:

```json
{
  "actions": [
    {
      "label": "Search DuckDuckGo",
      "command": "xdg-open",
      "args": ["https://duckduckgo.com/?q={text}"]
    },
    {
      "label": "Send to My Script",
      "command": "/home/user/bin/my-script",
      "args": ["--input", "{text}"]
    }
  ]
}
```

`{text}` is replaced with the selected text.
