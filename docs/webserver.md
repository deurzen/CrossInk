# Web Server Guide

This guide explains how to use CrossInk's built-in web server for file
transfer, device settings, Wi-Fi/OPDS management, and SD-card font management.

## Overview

The web server is available while the device is in **File Transfer** or
**Calibre Wireless** mode. It can:

- Upload, download, rename, move, and delete files on the SD card
- Create folders
- Edit many device settings from a browser
- Manage saved Wi-Fi networks and OPDS servers
- Browse, copy, and delete saved Word Inbox reading contexts
- Install, inspect, and remove compiled dictionary runtime packages through bounded APIs
- Upload and delete `.cpfont` SD-card font families
- Accept WebDAV clients and Calibre wireless uploads

The server does not require authentication. Use it only on trusted private
networks or in hotspot mode when you control who is connected.

## Starting File Transfer

1. From the Home screen, select **File Transfer**.
2. Choose one of the available modes:

| Mode | Use when |
|------|----------|
| **Join Network** | You want the reader to join an existing Wi-Fi network. |
| **Calibre Wireless** | You want to receive books from the CrossPoint Calibre plugin workflow. |
| **Create Hotspot** | You want the reader to create its own open Wi-Fi network. |

## Join Network Mode

1. Select **Join Network**.
2. Pick a 2.4 GHz Wi-Fi network from the scan results.
3. Enter the password if prompted.
4. Save credentials if you want the reader to reconnect automatically next time.

After connection, the reader shows:

- The connected SSID
- A QR code for the web URL
- The direct IP URL, for example `http://192.168.1.102/`
- The mDNS fallback URL, usually `http://crosspoint.local/`

Use either URL from a phone, tablet, or computer on the same network.

## Create Hotspot Mode

1. Select **Create Hotspot**.
2. Connect your phone or computer to the open Wi-Fi network:

```text
CrossPoint-Reader
```

3. Open the URL shown on the reader. `http://crosspoint.local/` is preferred
   when supported; the fallback IP is typically `http://192.168.4.1/`.

The reader displays one QR code for joining the hotspot and another QR code for
opening the web interface.

## Calibre Wireless Mode

Calibre Wireless starts the same web server in station mode, then displays setup
instructions and upload progress on the reader. Use this mode with the
CrossPoint Calibre plugin or other clients that speak the documented WebSocket
upload protocol.

For Calibre OPDS browsing, add `/opds` to the catalog URL when configuring an
OPDS server.

## Web Interface

The browser UI has six primary pages.

### Home

The Home page shows firmware status, network mode, IP address, device type,
uptime, and free heap.

### File Manager

The File Manager page can:

- Browse SD-card folders
- Upload files, using WebSocket upload when available and HTTP upload as a fallback
- Optimize EPUB files before upload
- Create folders
- Download files
- Rename files
- Move files into existing folders
- Delete one or more selected files or empty folders

Existing files with the same name are overwritten by uploads. When EPUB files
are overwritten, moved, renamed, or deleted through the web server, the matching
book cache is cleared so stale metadata is not reused.

#### EPUB Optimization

When uploading an EPUB, the upload dialog can optimize the file in the browser
before sending it to the reader. This is useful for image-heavy books that are
too large or memory-sensitive for the device.

The default optimization path converts images for e-ink reading, limits them to
the target device size, saves them as JPEG at 85% quality, and applies basic EPUB
repairs such as safer SVG handling. Advanced Mode lets you pick the target
device, JPEG quality, image split or rotation handling, split overlap, and
stable reference-page sizing.

**Add dictionary support** is a separate upload option from image optimization.
Select a `.cpdict` bundle once; the browser validates it and keeps only its
compiler metadata in IndexedDB. A Web Worker analyzes the EPUB's spine off the
UI thread, inserts 64-token source-shard markers, and embeds an uncompressed
`META-INF/crossink/language.bin`. When image optimization is disabled, all
images and auxiliary package files remain unchanged. Compiler data stays in the
desktop browser and is not sent over the reader's network connection. The EPUB
requires the matching runtime dictionary package on the reader.

Optimization changes the EPUB contents and therefore breaks hash-based KOReader
sync. Conversion is transactional: if image repair or dictionary compilation
fails or is cancelled, the original EPUB is not silently uploaded. Fix the
reported error, disable the failing option, or explicitly upload without
optimization.

### Settings

The Settings page exposes many firmware settings in the browser. It also has
cards for:

- Saved Wi-Fi networks
- OPDS servers

Passwords are accepted when adding or editing entries, but saved passwords are
not returned by the API.

### Word Inbox

The Word Inbox page groups captured reading contexts by book. It displays
chapter/page/progress metadata and copyable visible text for EPUB and
TXT/Markdown captures, plus a reader screenshot when that optional setting was
enabled. XTC/XTCH captures contain location metadata and an optional screenshot
because those formats contain pre-rendered pages rather than source text.

Use Previous/Next or the keyboard arrow keys to review captures. The browser
keeps a bounded cache of five contexts in each direction; context metadata and
text arrive in one response, while screenshots are loaded only for the visible
context. Per-book on-disk indexes keep navigation lookups independent of the
number of captures after a one-time migration rebuild. Individual contexts or
all contexts for a book can be deleted from this page. Word Inbox data remains
under `/.crosspoint/word_inbox/` when ordinary render caches are cleared.

### Dictionaries

The Dictionaries page installs `.cpdict` bundles and reports whether each
reader runtime has matching compiler data cached in the current desktop
browser. It extracts and uploads only the five runtime files; `forms.bin` stays
in IndexedDB for EPUB optimization. Files are uploaded into a hidden staging
directory and the package becomes visible only after exact-size, UUID, and CRC
validation. Installation can be cancelled, and replacing a bundle preserves
the previous runtime until commit. Removing a runtime deliberately retains its
global learning state. The same page reviews known, learning, ignored, and
implicitly familiar words, updates statuses through the WAL-protected store,
and exports the complete list as CSV or TSV in the desktop browser.

### Fonts

The Fonts page lists installed SD-card font families and lets you upload
`.cpfont` files. Upload files from one font family at a time. The server validates
the font family name, filename, and `.cpfont` magic bytes before accepting the
upload.

Installed fonts appear in **Settings > Reader > Font Family** after the font
registry refreshes.

## Command Line Use

Power users can use `curl`, WebDAV clients, or WebSocket clients while the web
server is running.

Endpoint details are documented in [webserver-endpoints.md](./webserver-endpoints.md).

## Security Notes

- The HTTP server runs on port 80.
- The WebSocket upload server runs on port 81.
- There is no authentication.
- Anyone on the same network can access the web interface while it is running.
- The server stops when you exit File Transfer or Calibre Wireless mode.
- Hotspot mode creates an open network for connectivity fallback; disconnect when done.

## Tips

1. Use **Create Hotspot** when no trusted network is available.
2. Prefer `crosspoint.local` when available, but keep the displayed IP address as a fallback.
3. Move closer to the router if upload progress stalls in Join Network mode.
4. Upload custom fonts through the Fonts page or copy them to `/.fonts/` or `/fonts/` on the SD card.
5. Exit File Transfer mode when finished to conserve battery.

## Related Documentation

- [User Guide](../USER_GUIDE.md)
- [Webserver Endpoints](./webserver-endpoints.md)
- [SD Card Fonts](./sd-card-fonts.md)
- [Troubleshooting](./troubleshooting.md)
