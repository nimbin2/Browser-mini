# browser-mini

A single-window WebKitGTK 6.0 / GTK4 page viewer in one C file. No tabs, no
toolbar, no bookmarks — a URL, a few keybindings, and a persistent cookie jar.

## Build

Dependencies on Debian and derivatives:

```sh
sudo apt install build-essential pkg-config \
     libgtk-4-dev libwebkitgtk-6.0-dev libsoup-3.0-dev
```

```sh
gcc -O2 -Wall -Wextra -o browser-mini browser-mini.c \
    $(pkg-config --cflags --libs gtk4 webkitgtk-6.0 libsoup-3.0)
```

## Usage

```sh
./browser-mini https://example.com
./browser-mini example.com --app-id mail --title Mail
./browser-mini ./local/page.html --css dark.css --private
```

A bare hostname gets `https://` prepended; an existing path is turned into a
`file://` URI.

Run `./browser-mini -h` for the full option list.

## Keys

`<mod>` is Ctrl unless you pass `--mod alt|super|meta`.

| key | action |
| --- | --- |
| `<mod>+R`, `F5` | reload |
| `<mod>+Shift+R` | re-read the `--css` file, then reload |
| `<mod>+D`, `F12` | toggle developer tools |
| `<mod>+O` | URL bar, top left — Enter loads, Esc cancels |
| `<mod>+P` | show the current URL, top right, for 4 seconds |
| `<mod>+plus` / `<mod>+minus` | zoom in / out |
| `<mod>+0` | reset zoom |
| `<mod>+Y` | copy the current URL |

Nothing else is intercepted, so `<mod>+A`, `<mod>+C` and `<mod>+V` reach the
page and keep working in input fields. While the URL bar is open every key
except Esc belongs to it.

Note that `Ctrl+P` shadows the page's print dialog; `--mod alt` avoids that.

## Window identity

`--app-id ID` sets the Wayland `app_id` (X11: `WM_CLASS`), default
`browser-mini`. This is what a compositor matches on:

```
for_window [app_id="mail"] floating enable
```

The title follows the page `<title>` by default. `--title NAME` pins it
instead, which is useful when a rule matches on the title. Adding
`--page-title` alongside `--title` keeps following the page and uses `NAME`
only as the fallback.

## Clipboard

`<mod>+Y` pipes the URL into `--clip-cmd`, `wl-copy` by default:

```sh
--clip-cmd "xclip -selection clipboard"   # X11
--clip-cmd internal                       # GTK's own clipboard
```

## Profiles

Cookies persist per profile, so logins survive restarts:

```sh
./browser-mini https://mail.example.com --profile mail
./browser-mini https://example.com --private      # ephemeral, stores nothing
./browser-mini https://example.com --clear-data   # wipe this profile first
```

Data lives in `~/.local/share/wkview/<profile>` and
`~/.cache/wkview/<profile>`. That directory name predates the program's
current name and was deliberately left alone so existing cookie jars keep
working — to rename it, change `PROFILE_DIR_NAME` in the source and move the
two directories to match.

## Downloads

Downloads go to the XDG download directory unless `--download-dir DIR` says
otherwise, and existing files are never overwritten — a counter is appended
instead. Responses with `Content-Disposition: attachment`, or with a MIME
type WebKit cannot render, are downloaded rather than displayed.

## Notes

Media permission requests (camera, microphone, screen share) are granted
automatically; `--no-media` denies them instead. Pass `--user-agent` or one of
the `--ua-*` shorthands for sites that sniff the browser. `-q` silences the
diagnostics on stderr.
