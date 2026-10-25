# Pod Files 1.3.0

A file manager for the SD card of the ClockworkPi PicoCalc (Pico 2 W; also builds for Pico / Pico 2), built on the PicoPods libraries.

Pod Files was called *PicoCalc Files*. Version 1.3.0 is the renamed 1.2.11: on first start it renames the old `/.picocalc_files` folder to `/.podfiles`, so settings, the saved Wi-Fi network and Bluetooth pairings carry over.

## What it does

- Browse the card, mark several entries, copy / cut / paste files and whole folders, delete, rename, create folders, see sizes.
- Name conflicts ask what to do: **Replace**, **Keep both** (`name (2).ext`), **Skip**, the same "for all", or **Cancel**. Folder onto folder offers **Merge**.
- Moves within the card are instant renames; copies keep the original date and attributes.
- Long operations show progress; **Esc** asks to stop (a half-copied file is removed).
- **Bluetooth** file transfer both ways with Android phones and Linux / Windows PCs.
- **Git clone** over Wi-Fi: any public repository into the current folder.
- **USB drive mode**: the SD card becomes a USB flash drive for a computer (**does not enumerate yet on real hardware**, under investigation; see the USB probe app).

## Keys

| Key | Action |
|---|---|
| Up / Down, PgUp / PgDn, Home / End | move |
| Enter or Right | open folder (on a file: action menu) |
| Backspace or Left | parent folder |
| Space | mark / unmark, then move down |
| `a` | mark all / none |
| `c` / `x` / `v` (or Ctrl+C / X / V) | copy / cut / paste here |
| `d` or Del | delete (asks first, with counts and size) |
| `r` or F2 | rename |
| `n` or F7 | new folder |
| `i` | info: size, files and folders inside, free space |
| `b` | Bluetooth: send / receive files |
| `g` | Git clone |
| `u` | USB drive mode |
| `h` or `.` | show / hide hidden files |
| `s` | sort by name, size, type |
| F1 or `?` | help |
| Esc | menu (also theme, list font, hidden files, sort) |

Copy, cut, delete and info act on the marked entries, or on the highlighted one when nothing is marked. The clipboard survives folder changes and USB drive mode; items that disappeared meanwhile are skipped.

## Bluetooth

Standard Bluetooth file transfer (OBEX Object Push), the "send via Bluetooth" of Android and desktop Linux. **iPhone and iPad do not support it.**

**Receive** (phone or PC -> PicoCalc): go to the destination folder, press `b`, *Receive files here*. The PicoCalc becomes visible as `PicoCalc XX:XX`.
- Android: share a file -> Bluetooth -> PicoCalc.
- Ubuntu: Settings -> Bluetooth -> PicoCalc -> *Send Files* (or `bluetooth-sendto`).

Files arrive in the folder (a name already there becomes `name (2).ext`). Esc stops receiving.

**Send** (PicoCalc -> phone or PC): mark files (or highlight one), press `b`, *Send*. Pick a paired device or *Search for devices* (keep the phone's Bluetooth settings open so it is visible). The phone asks to accept the file. Receiving on Ubuntu: keep Settings -> Bluetooth open while sending.

**Pairing** happens on the first transfer: the PicoCalc shows a 6-digit code; accept if the other device shows the same. *Paired devices...* lists them and can forget one. Pairing keys are stored in `/.podfiles/bt_keys.bin` on the card, never in the Pico's flash (so they cannot collide with the UF2 Loader).

Once used, Bluetooth stays on in the background until the PicoCalc is switched off, hidden and not accepting connections outside the Bluetooth screens (the radio chip does not restart reliably within a session, so it is never shut down and restarted).

Folders cannot be sent (Object Push carries single files). The receive screen shows a diagnostics line (`hci`, events, packets, last status) to help if something does not connect.

**Not yet tested on real hardware.** The receiving logic and the UI are tested in the simulator with real OBEX packets; the radio side follows BTstack's own APIs and examples.

## Git clone

1. Go to the folder that should contain the repository and press `g` (or *Git clone...* in the menu).
2. Type `owner/repo` (GitHub) or a full URL (`https://codeberg.org/owner/repo`, `https://gitlab.com/...`).
3. Choose **Latest commit** (fast, default) or **Full history**, and optionally a branch or tag.
4. The first time, pick your Wi-Fi network and type its password (saved in `/.podfiles/wifi.txt`, plain text). *Wi-Fi...* in the menu manages it; the radio turns off after 10 idle minutes.

The result is a real Git repository: the files plus a `.git` folder that `git status`, `git log` and `git pull` understand when the card is in a computer. Esc stops a clone; the half-made folder is removed.

Limits and notes:
- Public repositories only for now (no sign-in, no push).
- Up to 5000 objects per download (files + folders + commits): fine for most projects with *Latest commit*; large histories may exceed it.
- Names FAT cannot store (`:` `?` `*` ...) and submodules are skipped and counted.
- Certificates are checked against built-in public roots (GitHub, Let's Encrypt, DigiCert, Google, Amazon, GlobalSign). For a self-hosted server with its own CA, put the CA in PEM format in `/.podfiles/ca.pem`. The device has no clock, so certificate expiry dates are not checked.

## USB drive mode

1. Press `u` (or pick *USB drive mode* in the menu).
2. Connect the **Pico's micro-USB port** to the computer. (The PicoCalc's own USB-C port goes through a USB-serial chip and cannot act as a drive.)
3. The card appears as a normal USB drive (*PicoCalc SD card*): no driver needed on Windows, macOS or Linux.
4. When done, **eject** the drive on the computer: the PicoCalc goes back to the file list by itself.

While the mode is on, the file manager does not touch the card (it is unmounted), so the two sides can never write at the same time. Pressing Esc while the computer still has the drive open asks for confirmation, because data the computer has not flushed yet could be lost.

Speed is limited by the SD card's SPI bus: roughly 0.5-1 MB/s. The device uses the pid.codes test USB ID `1209:0001`; define `PD_USB_VID` / `PD_USB_PID` to use your own.

### If the computer does not see the drive

The USB screen shows two diagnostic lines; they tell how far the computer got:

| What you see | Meaning |
|---|---|
| `pull-up off` | the device is not announcing itself (firmware problem) |
| `pull-up on`, `irq` stays 0 after plugging in | nothing arrives from the computer: cable, port, or the Pico's USB not reaching the connector |
| `irq` grows, `desc dev` stays 0 | the computer talks but its requests are not understood |
| `desc dev` > 0, `addr 0` | replies do not reach the computer (signal quality: try another cable / port, no hub) |
| `addr` set, `cfg` 0 or `str` only | enumeration stopped half way |
| status *Connected* | USB works; any problem is on the storage side |

`task` must keep growing while the mode is on (the USB stack runs in the background). On Linux, `sudo dmesg -w` while plugging in shows the computer's side.

## Files it writes

`/.podfiles/settings.txt`: theme, list font, sort order, hidden files, last folder, last Git URL. `/.podfiles/wifi.txt`: Wi-Fi network and password. `/.podfiles/bt_keys.bin`, `bt_devices.txt`: Bluetooth pairing keys and device names.

## Building

Part of [carbo-pods](https://github.com/Pod-ium/carbo-pods): `cmake --build build --target podfiles` gives `build/pods/podfiles/podfiles.uf2`. Host simulator: `PD_SDCARD=./sd build-host/pods/podfiles/podfiles`, and `ctest -R podfiles` runs its tests (the `fs_ops` and `obex_receiver` library tests are in [hydrogenium-pods](https://github.com/Pod-ium/hydrogenium-pods)).

## Not yet tested on hardware

Everything was tested in the host simulator; the device build is clean but has not run on a PicoCalc. First checks worth doing: copy a large folder on the device, then a round trip through USB drive mode (copy a file from the computer, eject, open it on the PicoCalc).
