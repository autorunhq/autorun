#!/usr/bin/env python3
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_text(encoding="utf-8").split("#else /* __SWITCH__ */", 1)[0]

def extract(signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if not depth:
                return source[start:pos + 1] + "\n"
    raise RuntimeError(f"unterminated function: {signature}")

result = extract("NTSTATUS signal_set_full_context")
result += "\n"
result += extract("NTSTATUS call_user_exception_dispatcher")
result = result.replace(" struct thread_data *data,", "")
Path(sys.argv[2]).write_text(result, encoding="utf-8")
