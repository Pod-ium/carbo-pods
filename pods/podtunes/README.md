# Pod Tunes 1.0.0

An 8-bit music creator (chiptune tracker) for the ClockworkPi PicoCalc (Pico 2 W; also builds for Pico / Pico 2), built on the PicoPods libraries. Modelled on the "Pixel Plains" web tracker: the same four channels, patterns, order, waves and demo theme, with sound on the PicoCalc's PWM audio output.

## What it does

- **4 channels** — LEAD, ARP, BASS (piano-roll tracks, two octaves each) and DRM (kick / snare / hat rows).
- **8 patterns of 32 steps** (two bars of 16th notes) and a **song order** that plays them in sequence; a pattern mode loops the current pattern.
- **Waves** per track: square 25% / 50% / 12.5% and triangle; kick is a pitched sine drop, snare and hat are filtered noise.
- **Edit from the keyboard**: the letter keys play notes straight into the grid at the cursor (and sound them), arrows move the cursor.
- **Mute / solo** and a **volume** per channel, a **tempo** (60–240 BPM), an octave and a note length for entry.
- **Export the song as JSON** (the same format as the Pixel Plains web creator, so songs swap both ways), **import** such a JSON file from the card, and **render the song to a .wav file** (16-bit stereo, 22050 Hz) streamed straight to the card.
- The song is **autosaved** to `/.podtunes/song.json` a moment after every change and reloaded on start; the demo theme *PIXEL PLAINS ZONE* loads when there is none.
- A spectrum-style **visualiser** on top and per-channel **level bars** on the right follow the music.

## Keys

| Key | Action |
|---|---|
| Space or F2 | play / stop |
| Enter or F3 | play from the cursor |
| F4 | song / pattern mode |
| `[` `]` or F5 / F6 | previous / next pattern |
| F7 / F8 | copy pattern / clear pattern (asks first) |
| Tab or F9 | next track (LEAD ARP BASS DRM) |
| F10 | next waveform (SQ25 SQ50 SQ12 TRI) |
| `-` `=` | tempo down / up |
| `,` `.` | octave down / up |
| Arrows | move the cursor (Shift + Left/Right: 4 steps) |
| Backspace or Del | clear the cell |
| `Z S X D C V G B H N J M` | notes, lower octave (entered at the cursor, then advance) |
| `Q 2 W 3 E R 5 T 6 Y 7 U` | notes, upper octave |
| `I 9 O 0 P` | notes, two octaves up |
| on the DRM track: `Z` `X` `C` | kick / snare / hat (toggle at the cursor) |
| Ctrl+E | export the song as JSON |
| Ctrl+I | import a song (JSON) from the card |
| Ctrl+W | render the song to a .wav file (Esc cancels) |
| Ctrl+R | reset to the demo song (asks first) |
| F1 | help |
| Esc | menu: order, jump to pattern, mute/solo, per-channel volumes, tempo, song name, theme, master volume, about |

## Files it writes

`/.podtunes/settings.txt`: theme, master volume. `/.podtunes/song.json`: the autosaved song (JSON, compact). Exported songs and .wav files go wherever you type in the export prompt (default `/<song-name>.json` / `.wav`).

## Building

Part of [carbo-pods](https://github.com/Pod-ium/carbo-pods): `cmake --build build --target podtunes` gives `build/pods/podtunes/podtunes.uf2`. Host simulator: `PD_SDCARD=./sd build-host/pods/podtunes/podtunes` (with SDL2 installed it opens a window with sound), and `ctest -R podtunes` runs its scripted sessions.

## Not yet tested on hardware

Everything was tested in the host simulator (screens, keys, JSON/WAV export, import); the device build is clean but has not run on a PicoCalc. First checks worth doing: sound on the PWM output (volume, no crackle), the keyboard feel while playing, and a WAV render of the demo song on the card.
