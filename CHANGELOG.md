# Changelog

Pods of carbo-pods (productivity). Until the split of the PicoPods tree their
history was kept in Pod-ium/PicoPods; the libraries' changes are in the
CHANGELOG of hydrogenium-pods.

## Unreleased

### Repository
- Moved here from Pod-ium/PicoPods, with the pods' scripted host tests. Built
  on the libraries of hydrogenium-pods (`picopods_import.cmake`).
- CI (G-host, G-fw, G-web) on pushes and pull requests; release workflow on
  tags `v*` and published releases.

### Pods
- Pod Writer: **accent keys** for typing accented vowels (Italian and
  others). F10 (or Esc → Accent keys) turns them on and off; the choice is
  remembered. While on, `'` and `` ` `` are dead keys, as on the US
  International layout: `` ` `` then a vowel types à è ì ò ù (À È Ì Ò Ù),
  `'` then a vowel types á é í ó ú (Á É Í Ó Ú), and either key followed by
  Space types the plain character. The status bar shows `ACC`, and the
  pending key next to it.
- Pod Writer, Pod Files: `main()` ends with `return 0` (needed now that the
  host window renames it).

## PicoPods 1.0.0 (2026-10-24)

- **Pod Writer 3.0.0** (was PicoCalc Writer 3.0.0): `pods/podwriter`,
  `podwriter.uf2`, Wi-Fi host name `PodWriter`, ePub generator
  "Pod Writer". Data stays in `/.fwedit/`.
- **Pod Files 1.3.0** (was PicoCalc Files 1.2.11): `pods/podfiles`,
  `podfiles.uf2`. Work folder `/.podfiles/`; an existing
  `/.picocalc_files/` is renamed on first start.
