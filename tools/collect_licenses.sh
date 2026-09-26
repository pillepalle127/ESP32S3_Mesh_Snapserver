#!/bin/sh
# Prints the licences of everything linked into the firmware, for
# THIRD_PARTY_LICENSES.txt in each release: ESP-IDF (copied out of the
# build container into build/licenses/) and every managed component,
# including libraries vendored below it (libopus in micro-opus).
set -e

echo "Third-party licences of the SnapMesh firmware"
echo "SnapMesh itself is MIT licensed, see LICENSE in the repository."

print() {
    echo
    echo "===== $1"
    echo
    cat "$2"
}

for f in build/licenses/*; do
    [ -f "$f" ] && print "$(basename "$f")" "$f"
done

for d in managed_components/*/; do
    find "$d" -maxdepth 3 -type f \( -iname 'license*' -o -iname 'copying*' -o -iname 'notice*' \) \
        -not -path '*/test*' -not -path '*/doc*' | sort | while read -r f; do
        print "${f#managed_components/}" "$f"
    done
done
