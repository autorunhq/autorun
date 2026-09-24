#!/usr/bin/env python3
"""The profile directory has to be the one shell32 resolves.

dlls/shell32/shellpath.c does not expand %USERPROFILE% from the environment: in
_SHExpandEnvironmentStrings it builds every CSIDL_Type_User folder as
ProfilesDirectory (C:\\users by default) plus GetUserNameW(), which this Wine
answers with the name in dlls/advapi32/advapi.c. SHGetFolderPathW then refuses a
folder that is not on the card unless the caller passed CSIDL_FLAG_CREATE, and a
game that does not test that failure builds its path from an empty string -
FalloutNV read C:\\My Games\\FalloutNV\\FalloutPrefs.ini, found nothing, and
handed itself back to its launcher.

So: the runtime's profile must be named after GetUserName, its environment must
agree with it, the per-user folders shell32 precreates must be made, and the
settings the packager stages must sit inside that profile."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
runtime = (root / 'wine-nx-probe/source/runtime.c').read_text()
advapi = (root / 'dlls/advapi32/advapi.c').read_text()
shellpath = (root / 'dlls/shell32/shellpath.c').read_text()
packager = (root / 'wine-nx-probe/tools/package-wow64-full.py').read_text()
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)


# GetUserNameW reads WINEUSERNAME from the process environment.
check('GetEnvironmentVariableW( L"WINEUSERNAME"' in advapi,
      'GetUserNameW no longer reads WINEUSERNAME; re-read this test')
wine_user = re.search(r'"WINEUSERNAME=([^"\\]*)\\0"', runtime)
check(wine_user is not None, 'runtime environment no longer defines WINEUSERNAME')
user_name = wine_user.group(1) if wine_user else ''

# shell32 still builds the profile from that name rather than the environment.
check('GetUserNameW(userName, &userLen);' in shellpath,
      'shell32 no longer builds %USERPROFILE% from GetUserNameW; re-read this test')
check('if (!(nFolder & CSIDL_FLAG_CREATE))' in shellpath,
      'shell32 no longer refuses a missing folder; re-read this test')

profile = re.search(r'#define WINE_USER_DIR WINE_DRIVE_C "/users/([^"]*)"', runtime)
check(profile is not None, 'runtime.c no longer defines WINE_USER_DIR under /users')
if profile and user_name:
    check(profile.group(1) == user_name,
          f'the profile is /users/{profile.group(1)} but GetUserName answers {user_name!r}, '
          'so every CSIDL_Type_User folder resolves somewhere that does not exist')

# The environment a program reads names the same profile.
for name in ('APPDATA', 'LOCALAPPDATA', 'USERPROFILE'):
    value = re.search(r'"%s=([^"\\]*(?:\\\\[^"\\]*)*)\\0"' % name, runtime)
    check(value is not None, f'{name} is no longer in the runtime environment')
    if value and user_name:
        check(value.group(1).startswith('C:\\\\users\\\\%s' % user_name),
              f'{name} is {value.group(1)!r}, which is not under the profile')
# Anything else in the environment that names a folder under users\ names this
# profile: DXVK_CONFIG_FILE came in pointing at users\wine, where the runtime
# no longer writes the file it names, so DXVK would never have found it.
block = runtime[runtime.index('runtime_environment[] ='):]
block = block[:block.index(';')]
for folder in re.findall(r'users\\\\([^\\"]+)', block):
    check(folder == user_name, f'the environment names users\\{folder}, not the profile {user_name}')
check('"USERNAME=%s\\0"' % user_name in runtime,
      f'USERNAME does not answer {user_name!r}, the name GetUserName gives')
check('"HOMEPATH=\\\\users\\\\%s\\0"' % user_name in runtime,
      'HOMEPATH does not name the profile')

# Every per-user folder shell32 precreates is made, so the exists check passes.
made = set(re.findall(r'mkdir\( WINE_USER_DIR "/([^"]*)", 0777 \)', runtime))
for folder in ('AppData/Local', 'AppData/Roaming', 'Desktop', 'Documents',
               'Downloads', 'Music', 'Pictures', 'Saved Games', 'Videos'):
    check(folder in made, f'the runtime does not create {folder} in the profile')

# The settings the packager ships are inside that profile.
staged = re.search(r"stage / 'drive_c/users/([^/]*)/Documents/My Games/FalloutNV'", packager)
check(staged is not None, 'the packager no longer stages FalloutPrefs.ini under Documents')
if staged and user_name:
    check(staged.group(1) == user_name,
          f'FalloutPrefs.ini is staged under /users/{staged.group(1)}, which the game does not read')

if failures:
    for failure in failures:
        print('FAIL:', failure)
    sys.exit(1)
print(f'User profile: /users/{user_name} matches GetUserName, the environment, '
      f'the {len(made)} folders shell32 precreates and the staged game settings')
