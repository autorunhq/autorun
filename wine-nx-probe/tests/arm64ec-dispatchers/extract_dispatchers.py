#!/usr/bin/env python3
import ast
import re
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_text(encoding="utf-8").split("#else /* __SWITCH__ */", 1)[0]
output = []

if not re.search(r"\bULONG_PTR\s+wine_nx_do_syscall\s*\(", source):
    raise SystemExit("wine_nx_do_syscall must preserve pointer-sized return values")

for marker in ('.global __wine_syscall_dispatcher', '.global __wine_unix_call_dispatcher',
               '.global wine_nx_call_pe_callback'):
    pos = source.index(marker)
    start = source.rfind("__asm__(", 0, pos)
    end = source.index("\n);", pos) + 3
    block = source[start:end]
    strings = re.findall(r'"(?:\\.|[^"\\])*"', block)
    output.append("".join(ast.literal_eval(item) for item in strings))

output.append('.section .note.GNU-stack,"",%progbits\n')
Path(sys.argv[2]).write_text("".join(output), encoding="utf-8")
