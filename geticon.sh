#!/bin/bash
set -e

icon_dir="$HOME/.config/codexpopclip/icons"
mkdir -p "$icon_dir"

if [ -z "$1" ]; then
  echo "Usage: $0 <url-or-host> [icon-name]" >&2
  echo "Examples:" >&2
  echo "  $0 https://duckduckgo.com" >&2
  echo "  $0 duckduckgo.com" >&2
  echo "  $0 https://www.google.com google" >&2
  exit 1
fi

input="$1"
if [[ "$input" == *"://"* ]]; then
  SERVICE_URL="$input"
  host=$(printf '%s' "$input" | sed -E 's#^[a-zA-Z]+://##; s#/.*##')
else
  host="$input"
  SERVICE_URL="https://$input/favicon.ico"
fi

if [ -n "$2" ]; then
  ICONNAME="$2"
else
  ICONNAME="$host"
fi

ICONNAME=$(printf '%s' "$ICONNAME" | tr -cd 'A-Za-z0-9._-')
if [ -z "$ICONNAME" ]; then
  ICONNAME="icon"
fi

curl -L -o "$icon_dir/$ICONNAME.ico" "$SERVICE_URL"

if command -v convert >/dev/null 2>&1; then
  convert "$icon_dir/$ICONNAME.ico[0]" -resize 32x32 "$icon_dir/$ICONNAME.png"
elif command -v magick >/dev/null 2>&1; then
  magick "$icon_dir/$ICONNAME.ico[0]" -resize 32x32 "$icon_dir/$ICONNAME.png"
else
  echo "No ImageMagick found; keeping .ico files only." >&2
fi