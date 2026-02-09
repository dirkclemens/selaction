# selaction

Small Qt6 daemon-like app for KDE Plasma (Wayland) that watches clipboard/selection changes and shows a popup menu with text actions.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/selaction
```

## Autostart (KDE Plasma)

Create `~/.config/autostart/selaction.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=selaction
Exec=/home/user/Projekte/cpp/selaction/build/selaction
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
SELACTION_POLL=1 ./build/selaction
```

If Qt cannot read clipboard contents under Wayland, you can enable a fallback
using `wl-paste` (requires `wl-clipboard`):

```bash
SELACTION_POLL=1 SELACTION_WLPASTE=1 ./build/selaction
```

To reduce polling side effects (e.g. flicker), you can slow down polling and
limit wl-paste to primary selection only:

```bash
SELACTION_POLL=1 SELACTION_POLL_MS=1500 SELACTION_WLPASTE=1 \
  SELACTION_WLPASTE_MODE=primary ./build/selaction
```

`SELACTION_WLPASTE_MODE` accepts: `primary`, `clipboard`, `both`.

## Settings config

Create `~/.config/selaction/settings.json`:

```json
{
  "poll": true,
  "poll_ms": 1500,
  "wlpaste": true,
  "wlpaste_mode": "primary",
  "icons_per_row": 10,
  "log_level": "info"
}
```

Environment variables still work and override the file when set.

## External actions config

Create the config file:

`~/.config/selaction/actions.json`

Example:

```json
{
  "actions": [
    {
      "label": "Search DuckDuckGo",
      "command": "xdg-open",
      "args": ["https://duckduckgo.com/?q={text}"],
      "icon": "system-search"
    },
    {
      "label": "Send to My Script",
      "command": "/home/user/bin/my-script",
      "args": ["--input", "{text}"],
      "icon": "document-send"
    }
  ]
}
```

`{text}` is replaced with the selected text.
`icon` supports theme icon names (via `QIcon::fromTheme`), Qt standard pixmaps
with the `sp:` prefix (example: `sp:SP_ArrowUp`), or a local file icon via an
absolute path or `file://` URL (example: `/home/user/icons/google.png`).


to fetch icons from websites, you can use this script, but respect copyright!

```
set -e
icon_dir="$HOME/.config/selaction/icons"
mkdir -p "$icon_dir"

curl -L -o "$icon_dir/google.ico" "https://www.google.com/favicon.ico"
curl -L -o "$icon_dir/amazon.ico" "https://www.amazon.com/favicon.ico"
curl -L -o "$icon_dir/duckduckgo.ico" "https://duckduckgo.com/favicon.ico"

if command -v convert >/dev/null 2>&1; then
  for f in "$icon_dir"/*.ico; do
    convert "$f[0]" -resize 32x32 "${f%.ico}.png"
  done
elif command -v magick >/dev/null 2>&1; then
  for f in "$icon_dir"/*.ico; do
    magick "$f[0]" -resize 32x32 "${f%.ico}.png"
  done
else
  echo "No ImageMagick found; keeping .ico files only."
fi
```