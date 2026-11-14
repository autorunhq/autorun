#!/usr/bin/env python3
"""Rewrites the "Lines added" and "Lines found" cells of the patch table of
../PORTING.md from ledger.py, and prints the rows it changed. The notes of a
row are never touched: read them again when a row's numbers move.

    refresh_table.py            rewrite the table
    refresh_table.py --check    change nothing; fail when a row is out of date
"""
import re
import subprocess
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
porting = here.parent / 'PORTING.md'
row = re.compile(r'(\| (\d{4}) \| .*? \| )(\d+)( \| )(\d+)( % \|.*)')


def main():
    check = sys.argv[1:] == ['--check']
    if sys.argv[1:] and not check:
        sys.exit(__doc__)
    out = subprocess.run([sys.executable, str(here / 'ledger.py')],
                         stdout=subprocess.PIPE, text=True, check=True).stdout
    measured = {}
    for line in out.splitlines():
        number, added, found = line.split('|')[:3]
        measured.setdefault(number, []).append((added, found))
    lines = porting.read_text().split('\n')
    changed = 0
    for i, line in enumerate(lines):
        m = row.match(line)
        if not m:
            continue
        if not measured.get(m.group(2)):
            sys.exit('the table has a row %s that ledger.py does not know' % m.group(2))
        added, found = measured[m.group(2)].pop(0)
        new = m.group(1) + added + m.group(4) + found + m.group(6)
        if new != line:
            changed += 1
            print('  %s: %s lines, %s %% -> %s lines, %s %%' % (m.group(2), m.group(3), m.group(5), added, found))
        lines[i] = new
    left = [number for number, rest in measured.items() if rest]
    if left:
        sys.exit('ledger.py measured patches the table has no row for: %s' % ' '.join(left))
    if changed and not check:
        porting.write_text('\n'.join(lines))
    print('rows %s: %d' % ('out of date' if check else 'changed', changed))
    return 1 if check and changed else 0


if __name__ == '__main__':
    sys.exit(main())
