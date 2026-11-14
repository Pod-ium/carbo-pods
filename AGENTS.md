# AGENTS.md: carbo-pods

Instructions for an AI agent that adds features, fixes bugs or writes
documentation **in this repository**. Read this file and the README of the
pod you are changing before touching code.

## What this repository is

The **productivity pods** of the Pod-ium family, firmware for the
ClockworkPi **PicoCalc** with a Raspberry Pi **Pico 2 W** (also builds for
Pico 2, Pico W, Pico):

| Pod | Folder | Version | What it is |
|---|---|---|---|
| **Pod Writer** | `pods/podwriter` | 3.0.0 | Text editor: files larger than RAM (SD-backed), undo, syntax colours, ePub export, Send to X3, accent keys |
| **Pod Files** | `pods/podfiles` | 1.3.0 | File manager: copy / move / delete, Bluetooth, Git clone, USB drive mode |

The pods are built on the **PicoPods libraries** of
[hydrogenium-pods](https://github.com/Pod-ium/hydrogenium-pods)
(`picopods_import.cmake`: `PICOPODS_PATH`, otherwise a git fetch of its
`main`). Sibling pod repositories: helium-pods (games), aurum-pods
(emulators and the PicoPods Web page).

## Layout

```
pods/podwriter/   CMakeLists.txt, README.md, src/ (main.cpp, app.cpp, app.h, app_share.cpp, diag.h)
pods/podfiles/    CMakeLists.txt, README.md, src/main.cpp
tests/            CMakeLists.txt (scripted sessions), check_files.sh, podwriter_accents.txt
tools/            gate.sh, package_release.sh, package_test.sh, build_web_pods.sh, ci_ref.sh, ci_apt.sh
picopods_import.cmake
```

## What belongs here, and what doesn't

- Pod behaviour: the editor's commands, keys, rendering, menus; the file
  manager's screens and operations flow.
- **Not here**: the text engine (`pd_text`), ePub writer (`pd_epub`), file
  operations (`pd_fs`, `fs_ops`), Wi-Fi (`pd_wifi`/`pd_net`), CrossPoint
  client, Bluetooth (`pd_bt`), Git (`pd_git`), USB drive, dialogs (`pd_ui`).
  Bugs or features in those go to **hydrogenium-pods**; this repository then
  uses them. Do not copy library code into a pod.

## Rules for the pods

- Pods never name pins, SPI instances or chips; use the terminal and the
  libraries.
- Plain C++17, no Pico SDK headers outside what the libraries already allow;
  the pods must build for the PC host and the browser too.
- **Boards without Wi-Fi/BT must build**: link and call `pd_wifi`,
  `pd_crosspoint`, `pd_bt`, `pd_git` so that they report "not available"
  (Pod Writer F8/F9 say "No Wi-Fi on this board"). Guard with `PD_HAS_WIFI`,
  `PD_HAS_TLS`, `PD_HAS_BT` where needed.
- Long waits through `ui::Context::wait_key()` / the idle hook; sleeps
  through `pd::sys`.
- Memory is tight (no framebuffers; Pod Writer keeps documents on the SD
  card, not in RAM).
- **Every command is reachable from the Esc menu**; a key binding is
  optional. Check the pod's key table (README and code) for clashes before
  assigning a key.
- User choices that the pod already remembers (font, theme, status bar,
  syntax colours, accent keys…) go in its settings file.

### Data on users' SD cards (compatibility)
- **Pod Writer**: work folder `/.fwedit/` (settings `settings.txt`,
  clipboard, last file, cursor). The name is historical; **do not rename
  it**. Add new settings keys with a default when absent.
- **Pod Files**: work folder `/.podfiles/` (settings, `wifi.txt`, Bluetooth
  pairings); an old `/.picocalc_files/` is renamed once on first start
  (test `podfiles_migrate`). Keep that migration working.
- Wi-Fi host name `PodWriter`; ePub generator metadata "Pod Writer" (checked
  by `podwriter_script`).

## Build and gates

Toolchain (as CI): Pico SDK **2.1.1**, Arm GNU Toolchain **13.2.Rel1**,
CMake ≥ 3.13, Ninja, C++17 compiler, Python 3, `libmbedtls-dev`,
`libsdl2-dev`, `unzip`; Emscripten **3.1.74** for G-web.

| Gate | Command | Must |
|---|---|---|
| G-host | `PICO_SDK_PATH=… tools/gate.sh host` | PC build, every scripted session passes (the Bluetooth ones need `PICO_SDK_PATH`) |
| G-fw | `PICO_SDK_PATH=… tools/gate.sh fw pico2_w pico2 pico_w pico` | both pods link to `.uf2` on **all four** boards |
| G-web | `PICO_SDK_PATH=… tools/build_web_pods.sh` | both pods build for the browser (`dist/web-pods`) |

If you also changed hydrogenium-pods, add `PICOPODS_PATH=/path/to/hydrogenium-pods`.
Logs: `build-gate/*.log`.

Run a pod by hand on the PC:
`PD_SDCARD=./sd PD_FRAMES=./frames PD_KEYS="wait:200,text:hello,ctrl+s,shot" ./build-gate/host/pods/podwriter/podwriter`
(with SDL2 and no `PD_KEYS`, a window opens).

## Tests

Pod tests are **scripted sessions** in `tests/CMakeLists.txt`: the real pod
runs headless on a folder used as the SD card, driven by keys.

- Environment: `PD_SDCARD` (card folder), `PD_KEYS` (script), `PD_FRAMES`
  (where `shot` saves PPM frames), `PD_BT_SIM_*` (simulated Bluetooth).
- Key tokens, comma-separated: `enter esc bksp space up down left right
  f1…f12 ctrl+x shift+x`, a single character, `text:<string>`, `wait:<ms>`,
  `shot`.
- Start from a clean folder (`rm -rf …`), set `TIMEOUT`, and **assert on the
  result**: files on the card (`test -s`, `cmp` against an expected file in
  `tests/`, `grep` in settings), a checker script like `check_files.sh`, or
  `PASS_REGULAR_EXPRESSION`.
- Put `wait:` after operations that finish asynchronously: Pod Files drops
  type-ahead when an operation ends.
- One or two comment lines above each test: what it drives, what it checks.
- Feature ⇒ a session that uses it and checks its effect. Bug fix ⇒ a
  session (or a case in an existing one) that failed before the fix.

## Documentation

- **Pod README** (`pods/<pod>/README.md`, English): title with version
  (`# Pod Writer 3.0`), pitch, *What's new*, *Features* bullets, the **Keys**
  table (`| Key | Action |`), build notes. Keep the Keys table in sync with
  the code and the Esc menu.
- **Root README**: pod table, build, CI and releases, layout. Its Build / CI /
  Layout sections are parallel to the sibling repositories'; keep the wording
  aligned.
- **CHANGELOG.md**: add to `## Unreleased` → `### Pods` (or `### Repository`
  for build/CI). Start with the pod name, bold the feature, say what the user
  sees, the keys and any remembered setting (see the accent-keys entry).
  Library changes are recorded in hydrogenium-pods' changelog, not here.
- **Versions**: bump `VERSION` in the pod's `picopods_add_pod(...)` and its
  README heading together, only when asked or when preparing a release
  (feature → minor, fix → patch). Release assets come from
  `tools/package_release.sh` (`podwriter_<v>.uf2`, `podfiles_<v>.uf2`,
  `SHA256SUMS_<v>.txt`).
- Style: British spelling (colour, behaviour), terse, exact product names
  (Pod Writer, Pod Files, PicoPods, PicoCalc, Pico 2 W). Old names (PicoCalc
  Writer, PicoCalc Files) only as provenance ("was …").
- Verify every key, path and command you document against the code.

## Adding a pod here
`pods/<pod>/` from hydrogenium-pods' `examples/template`; `add_subdirectory`
in `CMakeLists.txt`; a scripted test; add it to `pods=` in
`tools/package_release.sh` and `tools/build_web_pods.sh`; README pod table
here, in hydrogenium-pods' README and aurum-pods' README; a description in
aurum-pods' `web/app.js`; changelog entry.

## Git

- Work on the branch you are given; push to `main` only when explicitly told
  to. Open a pull request only when asked.
- A change that also needs hydrogenium-pods uses **the same branch name**
  there: CI (`tools/ci_ref.sh`) then builds against that branch. The library
  change must reach hydrogenium-pods `main` before this one merges.
- Commit subject `<Pod or area>: <imperative summary>` (`Pod Writer: …`,
  `Pod Files: …`, `CI: …`, `Docs: …`), wrapped body with what and why and
  the new test's name. Add the attribution lines your harness requires. No
  model identifiers in commits, code or docs.
- Never skip, disable or loosen a test or CI step to get green.

## Done means

- [ ] Change is in the right repository (pod here, generic code in hydrogenium-pods).
- [ ] Scripted test added or extended.
- [ ] G-host, G-fw (4 boards), G-web pass.
- [ ] Pod README (features, keys), CHANGELOG updated and verified.
- [ ] SD-card data stays compatible.
- [ ] Report: what changed (`file:line`), gates and results, what needs a
      check on a real PicoCalc (display, keyboard feel, Wi-Fi/BT radio, USB).
