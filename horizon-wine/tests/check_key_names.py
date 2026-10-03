#!/usr/bin/env python3
"""Check the virtual-key names shared by input profiles and the launcher."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "key_names.h"

int main(void)
{
    for (int i = 0; i < WINE_NX_KEY_NAME_COUNT; i++)
    {
        assert(wine_nx_key_names[i].code <= 0xff);
        assert(wine_nx_key_index(wine_nx_key_names[i].code) == i);
        for (int j = i + 1; j < WINE_NX_KEY_NAME_COUNT; j++)
        {
            assert(wine_nx_key_names[i].code != wine_nx_key_names[j].code);
            assert(strcmp(wine_nx_key_names[i].name, wine_nx_key_names[j].name));
        }
    }
    assert(!strcmp(wine_nx_key_names[wine_nx_key_index(0x57)].name, "W"));
    assert(!strcmp(wine_nx_key_names[wine_nx_key_index(0x0d)].name, "Enter"));
    assert(wine_nx_key_index(0) == 0);
    assert(wine_nx_key_index(0xf5) == -1);
    printf("%d unique virtual-key names\n", WINE_NX_KEY_NAME_COUNT);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'keys.c'
    source.write_text(fixture)
    binary = Path(tmp) / 'keys'
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'horizon-wine/source'),
                    '-o', str(binary), str(source)], check=True)
    subprocess.run([str(binary)], check=True)
