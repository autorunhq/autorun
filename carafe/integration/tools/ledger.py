#!/usr/bin/env python3
"""The measure behind the patch table of ../PORTING.md.

For each of Carafe's Autorun patches: of the lines it adds (12 characters or
longer without their spaces), how many are in this tree's file today, ignoring spaces. With
--review, each line that is not found is looked up in Carafe's own final file
(the commit its whole patch set makes): a line that is not there either was
rewritten by a later Carafe patch ("superseded"); one that is there is a real
difference, to be read in the code.

    ledger.py                         the table's two numbers
    ledger.py --review [-v] [NNNN ...]
    --patches DIR    the patches, default ../patches-from-carafe
    --final REV      Carafe's final tree, default refs/carafe/patches
                     (git fetch origin carafe-patches:refs/carafe/patches)
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[3]


def squeeze(line):
    return re.sub(r'\s+', '', line)


def added_lines(patch):
    """{file: [added lines]} of a patch, without the +++ headers."""
    files, current = {}, None
    for line in patch.read_text(errors='replace').splitlines():
        if line.startswith('+++ '):
            name = line[4:].split('\t')[0]
            current = name[2:] if name.startswith('b/') else name
            files.setdefault(current, [])
        elif line.startswith('+') and current is not None:
            if len(squeeze(line[1:])) >= 12:
                files[current].append(line[1:])
    return files


def lines_of(text):
    return {squeeze(line) for line in text.splitlines()}


def final_file(rev, name, cache={}):
    if (rev, name) not in cache:
        run = subprocess.run(['git', '-C', str(root), 'show', '%s:%s' % (rev, name)],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        cache[(rev, name)] = lines_of(run.stdout.decode(errors='replace')) if run.returncode == 0 else None
    return cache[(rev, name)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--patches', default=str(Path(__file__).resolve().parents[1] / 'patches-from-carafe'))
    parser.add_argument('--review', action='store_true')
    parser.add_argument('-v', action='store_true')
    parser.add_argument('--final', default='refs/carafe/patches')
    parser.add_argument('numbers', nargs='*')
    args = parser.parse_intermixed_args()

    patches = sorted(Path(args.patches).glob('*.patch'))
    if not patches:
        sys.exit('no patches in %s' % args.patches)
    if args.review and final_file(args.final, 'dlls/ntdll/unix/horizon.c') is None:
        sys.exit('%s is not here; see --final in the help' % args.final)
    here = {}
    for patch in patches:
        number, slug = patch.stem.split('-', 1)
        if args.numbers and number not in args.numbers:
            continue
        added = found = superseded = 0
        real = []
        for name, lines in added_lines(patch).items():
            if name not in here:
                path = root / name
                here[name] = lines_of(path.read_text(errors='replace')) if path.is_file() else set()
            final = final_file(args.final, name) if args.review else None
            for line in lines:
                added += 1
                if squeeze(line) in here[name]:
                    found += 1
                elif args.review and final is not None and squeeze(line) not in final:
                    superseded += 1
                else:
                    real.append((name, line.strip()))
        if args.review:
            print('%s added=%d superseded=%d real=%d %s' % (number, added, superseded, len(real), slug))
            if args.v:
                for name, line in real:
                    print('     R %s: %s' % (Path(name).name, line))
        else:
            print('%s|%d|%d|%s' % (number, added, 100 * found // added if added else 100, slug))


if __name__ == '__main__':
    main()
