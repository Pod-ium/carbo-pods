#!/bin/sh
# Checks the SD folder left by the podfiles_script test (Pod Files).
set -e
sd="$1"
fail() { echo "check_files: $1" >&2; exit 1; }
[ -d "$sd/Crate" ]                      || fail "Box was not created/renamed to Crate"
[ ! -e "$sd/Box" ]                      || fail "Box still exists"
[ "$(cat "$sd/Crate/Docs/a.txt")" = hello ] || fail "Docs was not moved into the folder"
[ ! -e "$sd/Docs" ]                     || fail "cut left Docs in place"
[ "$(cat "$sd/Crate/b.txt")" = world ]  || fail "b.txt was not copied"
[ "$(cat "$sd/Crate/b (2).txt")" = world ] || fail "keep-both copy missing"
[ "$(cat "$sd/b.txt")" = world ]        || fail "copy source was removed"
[ ! -e "$sd/c.txt" ]                    || fail "c.txt was not deleted"
[ -f "$sd/.podfiles/settings.txt" ]      || fail "settings not saved"
echo "check_files: ok"
