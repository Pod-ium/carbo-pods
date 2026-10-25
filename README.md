# carbo-pods

[![CI](https://github.com/Pod-ium/carbo-pods/actions/workflows/ci.yml/badge.svg)](https://github.com/Pod-ium/carbo-pods/actions/workflows/ci.yml)

**Productivity** pods for the **ClockworkPi PicoCalc** with a Raspberry Pi
**Pico 2 W**, built on the PicoPods libraries.

| Pod | Folder | What it is |
|---|---|---|
| **Pod Writer** 3.0 | [`pods/podwriter`](pods/podwriter) | Text editor: files larger than RAM (SD-backed), undo, syntax colours, ePub export |
| **Pod Files** 1.3 | [`pods/podfiles`](pods/podfiles) | File manager: copy / move / delete, Bluetooth, Git clone, USB drive mode |

Part of the Pod-ium family of PicoCalc firmwares: [hydrogenium-pods](https://github.com/Pod-ium/hydrogenium-pods)
(libraries and examples), [helium-pods](https://github.com/Pod-ium/helium-pods) (games),
[carbo-pods](https://github.com/Pod-ium/carbo-pods) (productivity),
[aurum-pods](https://github.com/Pod-ium/aurum-pods) (emulators, PicoPods Web).

## Build

The libraries come from [hydrogenium-pods](https://github.com/Pod-ium/hydrogenium-pods):
`picopods_import.cmake` uses `-DPICOPODS_PATH=/path/to/hydrogenium-pods` (or
the environment variable) when given, otherwise CMake fetches them with git
into the build tree (`-DPICOPODS_FETCH_FROM_GIT_TAG=`, default `main`).

```sh
# Firmware for PicoCalc + Pico 2 W (default)
cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build -j
# -> build/pods/<pod>/<pod>.uf2

# Other boards
cmake -B build-pico2 -DPD_BOARD=pico2       # also pico, pico_w: on boards without Wi-Fi, F8/F9 and Wi-Fi features say so

# PC build: simulator + the pods' scripted sessions
cmake -B build-host -DPD_TERMINAL=host -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build-host -j && ctest --test-dir build-host
# ...and with SDL2 installed, the pod runs in a window (keyboard, sound):
PD_SDCARD=./sd ./build-host/pods/podwriter/podwriter

# The pods for the browser (load them into PicoPods Web like cartridges)
PICO_SDK_PATH=/path/to/pico-sdk tools/build_web_pods.sh   # -> dist/web-pods

# Both merge gates in one go
PICO_SDK_PATH=/path/to/pico-sdk tools/gate.sh all
```

Needs Pico SDK 2.1.x, `arm-none-eabi-gcc` and CMake ≥ 3.13; the host build
needs a C++17 compiler, Python 3 and `libmbedtls-dev` (and `libsdl2-dev` for
the window). Flash a `.uf2` in BOOTSEL mode, or copy it to `pico2-apps/` of
the SD card and start it from [UF2 Loader](https://github.com/pelrun/uf2loader).

## CI and releases

- **Pull requests and pushes** (`.github/workflows/ci.yml`): G-host (PC build
  + 5 scripted sessions; the Bluetooth ones need `PICO_SDK_PATH`), G-fw (`pico2_w`, `pico2`, `pico_w`, `pico`; `.uf2` files kept as artifacts
  `carbo-<board>`) and G-web (artifact `carbo-web-pods`).
- **Tags and releases** (`.github/workflows/release.yml`): pushing a tag
  `v*` creates its GitHub release, and publishing a release from the UI
  works too; either way `podwriter_<version>.uf2`, `podfiles_<version>.uf2` and `SHA256SUMS_<version>.txt` are attached
  (tag `v1.2.0` gives version `1.2.0`). Locally:
  `PICO_SDK_PATH=… tools/package_release.sh <version>`. It can also be run
  by hand from the Actions tab for an existing tag.
- **Which libraries.** CI and releases build against hydrogenium-pods
  `main`, or the ref in the repository variable `PICOPODS_REF` (set it to a
  hydrogenium-pods tag to make releases reproducible). On a branch, CI uses
  the hydrogenium-pods branch of the same name when it exists, so a change
  across both repositories is tested together.

## Layout

```
pods/                   podwriter/ podfiles/
tests/                  the pods' scripted host sessions (ctest), check_files.sh
tools/                  gate.sh (merge gates), package_release.sh (release .uf2),
                        build_web_pods.sh (.pod.js), ci_ref.sh (CI: sibling repository ref)
picopods_import.cmake   finds the PicoPods libraries (hydrogenium-pods)
```

See [`CHANGELOG.md`](CHANGELOG.md) for what changed.
