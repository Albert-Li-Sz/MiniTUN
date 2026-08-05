#!/usr/bin/env python3
"""Strip git archive's pax_global_header entry from a tar file.

git archive prepends a pax global header containing the archived commit id,
which makes the resulting archive's digest depend on the commit hash. This
helper removes that single entry so the remaining tar is a pure function of
the archived tree and its entry metadata.
"""

import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT_TAR OUTPUT_TAR", file=sys.stderr)
        return 2
    _, input_path, output_path = sys.argv
    with open(input_path, "rb") as input_file:
        data = input_file.read()
    if len(data) >= 512:
        name = data[0:100].split(b"\0", 1)[0]
        if name == b"pax_global_header":
            size_field = data[124:136].strip(b" \0")
            size = int(size_field or b"0", 8)
            entry_length = 512 + ((size + 511) // 512) * 512
            data = data[entry_length:]
    with open(output_path, "wb") as output_file:
        output_file.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
