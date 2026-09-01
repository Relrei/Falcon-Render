# SPDX-FileCopyrightText: 2026 Falcon Render
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Print a hash of the pixels of each image and check they all agree.

    python3 tools/falcon_evict_viewport_pixelhash.py a.png b.png ...

Used by the FALCON_CYCLES_EVICT_VIEWPORT gate to compare renders across
binaries and switch settings.

NOTE: the *file* cannot be hashed. Blender always writes the render time and the
date into the PNG metadata, so two byte-for-byte identical pictures never hash
the same as files.

Exit code 0 = all identical, 1 = not.
"""

import hashlib
import sys

import numpy as np
from PIL import Image


def main():
    paths = sys.argv[1:]
    if not paths:
        print("usage: falcon_evict_viewport_pixelhash.py IMAGE [IMAGE ...]", file=sys.stderr)
        return 2

    digests = []
    for path in paths:
        pixels = np.asarray(Image.open(path).convert("RGBA"))
        digest = hashlib.sha256(pixels.tobytes()).hexdigest()
        digests.append(digest)
        print("    %s  %s" % (digest, path))

    if len(set(digests)) == 1:
        print("  bitcheck: pixel identical")
        return 0
    print("  FAIL: the reference render is not identical across those binaries")
    return 1


if __name__ == "__main__":
    sys.exit(main())
