#!/usr/bin/env python3
"""impulse: corrupt a recorded session for the verifier's negative test.

Usage: tamper-session.py <in-session> <out-session> [field]

field: final (default) flips a byte of the recorded final-state hash,
       initial flips a byte of the recorded initial-state hash,
       cmds   flips a byte inside the command log.
"""
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 1
    field = sys.argv[3] if len(sys.argv) > 3 else "final"
    data = bytearray(open(sys.argv[1], "rb").read())
    if field == "final":
        off = 76
    elif field == "initial":
        off = 44
    elif field == "cmds":
        off = 108 + 4  # inside the first command's step field
    else:
        print(f"unknown field: {field}", file=sys.stderr)
        return 1
    if off >= len(data):
        print("session too short", file=sys.stderr)
        return 1
    data[off] ^= 0x01
    open(sys.argv[2], "wb").write(bytes(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
