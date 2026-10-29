# Pod Writer 3.0

A distraction-free, bare-metal C++ text editor firmware for the **ClockworkPi PicoCalc** with a **Raspberry Pi Pico 2 W** (RP2350).
Pod Writer was called *PicoCalc Writer*; it is the same editor, renamed when it joined PicoPods.
It boots straight into the last file you were editing, at the same cursor position, and gets out of your way.

Files are edited **directly on the SD card**, so documents far larger than the Pico's 520 KB of RAM (up to 2 GB) open instantly and edit smoothly.

## What's new in 3.0

- **Rebuilt on the PicoPods libraries.** The editor engine, display driver, fonts, dialogs, keyboard, SD card, Wi-Fi, ePub export and X3 client now come from the shared libraries ([hydrogenium-pods](https://github.com/Pod-ium/hydrogenium-pods)), the same code PodBoy, Pod Files and any new pod use. Only the editor itself (state, key bindings, rendering, commands) lives in this folder: 3 source files instead of 20.
- **Runs on every Pico-format board.** Pico 2 W is still the target; on boards without Wi-Fi (Pico, Pico 2) everything works except F8/F9, which report "No Wi-Fi on this board".
- **Runs on a PC.** The PicoPods host build runs the real editor headless with scripted keys and screenshots; it replaces 2.1's `test/appsim`.
- **Help** is a scrollable page; **Wi-Fi** connection progress and errors are shown in a panel (the PicoPods Wi-Fi manager) instead of the status line.
- Settings, Wi-Fi credentials and the work folder are unchanged (`/.fwedit/`), so an existing SD card works as before.

## Features

- New, Open (file browser), Save, Save As (with overwrite confirmation), unsaved-changes prompts
- Undo / redo (typing grouped by word), cut / copy / paste of any size (clipboard lives on the SD card)
- Find, find next, replace all (single undo step), go to line, document statistics (words, lines, chars)
- Soft word-wrap, UTF-8 aware (Latin-1 glyphs displayed), auto-indent, insert / overwrite mode
- Four Terminus font sizes (6×12, 8×16, 10×20, 12×24), three themes (Night, Paper, Phosphor)
- ePub export (Markdown becomes real headings, emphasis, lists, quotes, tables and code; chapters and table of contents from `#` / `##` headings)
- Wi-Fi (Pico 2 W) and one-key upload of the ePub to an Xteink X3 / X4 running CrossPoint
- Syntax colours for Markdown, XML (also URDF, xacro, SDF, SVG, HTML), Python and C/C++, chosen by file extension; F6 toggles them
- Esc menu with every command, and the display settings as Left/Right choices
- Optional one-line status bar: file name, modified mark, language, OVR, position %, battery
- Remembers font, theme, status bar, syntax colours, current folder, last file and cursor position

## Keys

| Key | Action |
|---|---|
| Esc | Menu (all commands) |
| F1 | Help |
| F2 / F4 / F5 | Font size / status bar / theme |
| F3 | Find next |
| F6 | Syntax colours on / off |
| F7 / F8 / F9 | Export ePub / Send to X3 / Wi-Fi |
| Ctrl+N / Ctrl+O / Ctrl+S | New / Open / Save |
| Ctrl+Shift+S | Save as |
| Ctrl+F / Ctrl+R / Ctrl+L | Find / Replace all / Go to line |
| Ctrl+Z / Ctrl+Y | Undo / Redo |
| Ctrl+A / C / X / V | Select all / Copy / Cut / Paste |
| Ctrl+T | Statistics |
| Shift+arrows | Select |
| Ctrl+Left / Right | Word left / right |
| Ctrl+Up / Down | Start / end of document |
| Alt+Left / Right / Up / Down | Home / End / Page up / Page down |
| Home, End, PgUp, PgDn, Del, Ins | As labelled (Ins toggles overwrite) |
| Tab | Inserts 4 spaces |

## ePub export and the Xteink X3

**F7 – Export ePub** writes `name.epub` next to the document (untitled documents go to the current folder). You confirm the title, which defaults to the first `#` heading. The document is converted according to its type:

- **Markdown** (`.md`): headings, bold/italic/strikethrough, inline code, links, lists (nested, numbered, task boxes), block quotes, tables, fenced code and horizontal rules. `#` and `##` headings start new chapters and form the table of contents.
- **Plain text**: each line is a paragraph, which suits the editor's soft wrap.
- **Source code** (`.py`, `.cpp`, `.xml`...): monospaced lines with indentation kept.

Long chapters are split every ~48 KB so the reader never gets a huge XHTML file. The output is EPUB 2.0.1, which passes `epubcheck`. Author and book language are set in **Esc → ePub & X3 settings**.

**F8 – Send to X3** exports the book, then uploads it to the reader:

1. On the X3 (CrossPoint firmware), open **File Transfer** and choose **Join network**, so it joins the same Wi-Fi as the PicoCalc.
2. On the PicoCalc press **F8**. The first time, pick your network from the scan and type the password (Tab shows it).
3. The book lands in the X3 folder set in the settings (default `/`). If a book with the same name is already there, you are asked before it is replaced.

The X3 address defaults to `http://crosspoint.local/`, resolved with mDNS. If your network blocks mDNS, set the IP address the X3 shows on its screen instead. The last address that worked is also remembered as a fallback. **Test X3 connection** in the settings asks the reader for its status.

**F9 – Wi-Fi** shows the connection and lets you choose another network, turn the radio off, or forget the saved network. The radio is only switched on when needed and turns itself off after 10 idle minutes.

**Privacy note:** the Wi-Fi name and password are stored in plain text in `/.fwedit/wifi.txt` on the SD card, so anyone with the card can read them.

## Building

Pod Writer is one of the pods of the PicoPods tree. From the repository root:

```sh
cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk        # PicoCalc + Pico 2 W
cmake --build build -j --target podwriter
# -> build/pods/podwriter/podwriter.uf2
```

Other boards: add `-DPD_BOARD=pico2` or `-DPD_BOARD=pico`. PC build: `cmake -B build-host -DPD_TERMINAL=host`, then for example

```sh
PD_SDCARD=./sd PD_FRAMES=./frames PD_KEYS="text:Hello,ctrl+s" ./build-host/pods/podwriter/podwriter
```

Requirements: Pico SDK 2.x, `arm-none-eabi-gcc`, CMake ≥ 3.13. FatFs is part of the PicoPods libraries; nothing is downloaded at configure time.

## Flashing

Hold **BOOTSEL** on the Pico while connecting USB and copy the `.uf2` onto the `RP2350` drive.
If you use a PicoCalc SD-card bootloader (e.g. the community UF2 loader), copy the `.uf2` into its firmware folder on the SD card instead.

The SD card must be FAT32 or exFAT. The editor creates a hidden `/.fwedit/` folder for its scratch files and settings.

## How it works

- **PicoPods libraries.** `src/app.cpp` (editor, rendering, commands), `src/app_share.cpp` (ePub, X3, Wi-Fi wrappers) and `src/main.cpp` are all that is specific to Pod Writer. The piece table, layout and syntax lexer are `pd_text`; the rest comes from `pd_gfx`, `pd_ui`, `pd_input`, `pd_fs`, `pd_wifi`, `pd_epub`, `pd_crosspoint` and `pd_diag`. See `docs/MIGRATION.md` in [hydrogenium-pods](https://github.com/Pod-ium/hydrogenium-pods) for the mapping from the 2.1 files.
- **Piece table on SD.** The opened file is never loaded into RAM: it is read in place, and everything you type is appended to `/.fwedit/add.tmp` (with a 4 KB RAM tail). The document is a list of 8-byte pieces pointing into those two files, backed by a 16 KB page cache.
- **Undo** stores piece-list snapshots of the changed region, not text, so undoing a huge paste or replace costs almost nothing.
- **Saving** writes a complete new file (`name.~fw`), then replaces the original and reloads it. It is crash-safe against partial writes, but it clears the undo history.
- **Layout** is computed on demand from the start of the current logical line, so there is no line index to build when opening a big file.
- **Rendering** uses a single-row line buffer with per-row hashing, so only rows that changed are resent to the LCD. There is no framebuffer.
- **Wi-Fi** uses the CYW43 driver with lwIP in poll mode: nothing runs in the background, and the network is pumped only by the main loop and while an operation waits. Uploads stream the ePub from the SD card straight into TCP as a `multipart/form-data` POST to CrossPoint's `/upload` endpoint.
- **Syntax colours** come from a small streaming lexer (`lib/content/text/src/syntax.cpp`) that only ever sees the visible rows. Everything that can span lines (block comments, docstrings, fenced code, multi-line XML tags) lives in a ~40-byte state that is cached at the top of the screen. That state stays valid while you type below it, and scrolling down continues from it. When it has to be rebuilt (jumping around, or editing above it) the lexer re-reads at most 8 KB before the top of the screen (`syn::kBackScan`). A construct opened further back than that, such as a code fence 8 KB above, may be coloured wrongly until you scroll closer.
- RAM use is about 107 KB static (most of it the Wi-Fi driver and lwIP buffers) plus the piece table; the firmware is about 530 KB of flash. After a very long editing session the table can fill up; the editor warns you to save, which compacts it.

## Start-up diagnostics

While starting, the firmware prints a short log (version, reset reason, and each step: display, keyboard, SD card, scratch files, settings, last file). The last step reached is also kept in a watchdog scratch register, so if a start is interrupted by a reset, the next start shows `previous start stopped at: ...`. If that step was reopening the last file, the editor skips it once (safe start). HardFaults and panics show a red crash screen with the program counter instead of locking up silently.

## Notes and limits

- New lines are inserted as LF; existing CRLF files keep their CRLF line endings. Tabs in existing files are shown as one cell.
- Maximum document size is 2 GB.
- Hardware tunables are the PicoCalc terminal options, set in `CMakeLists.txt` with `target_compile_definitions`: `PD_PICOCALC_LCD_HZ`, `PD_PICOCALC_LCD_INVERT` (0 if colours look inverted), `PD_PICOCALC_KBD_GAP_US` (16 ms by default), `PD_PICOCALC_SD_HZ`.
- Tests live in the top-level `tests/` (run with `ctest` in a host build): the randomized editing-engine test with ASan/UBSan, the lexer tests, the ePub samples with structure checks, the X3 upload against a mock CrossPoint server, and a scripted session of this app. Versions 1.0 to 2.1 ran on real PicoCalc hardware; **3.0 has been built and tested on a PC only so far**.

## Licences

- Terminus font: SIL Open Font License 1.1 (converted with `tools/gen_fonts.py` of hydrogenium-pods)
- FatFs: ChaN's BSD-style licence
