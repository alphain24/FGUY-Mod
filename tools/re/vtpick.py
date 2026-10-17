"""Research: how many objects of the given classes (vtables) a running game has now.

  vtpick.py <pid> <vtable hex> [<vtable hex> ...]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader  # noqa: E402
from vtcount import count  # noqa: E402


def main():
    pid = int(sys.argv[1])
    h, read = open_reader(pid)
    now = count(h, read)
    for v in sys.argv[2:]:
        objs = now.get(int(v, 16), [])
        print('%s: %d  %s' % (v, len(objs), ' '.join('%08x' % a for a in objs[:6])))


if __name__ == '__main__':
    main()
