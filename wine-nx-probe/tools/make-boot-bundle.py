#!/usr/bin/env python3
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


ATMOSPHERE_ZIP_SHA256 = "29ce59ae237be59cd20c15fff46cd1f5e127ec2b607e65a3412e396797448649"
PACKAGE3_SHA256 = "f162a419887374028103e097dc5679f97b3b22501fee667405a3bc965eeaa3f2"
HOC_ZIP_SHA256 = "48a57eef49032cbae615074676df4a5fb017aff40be3f45734a158d5b19ffa00"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def hash_initializer(data):
    checksum = digest(data)
    return "{" + ", ".join("0x" + checksum[i:i + 2] for i in range(0, 64, 2)) + "}"


def kip_loader(data):
    if len(data) < 256 or data[:4] != b"KIP1" or int.from_bytes(data[16:24], "little") != 0x0100000000000001:
        raise ValueError("Expected a loader KIP")


def payload(data):
    return "{" + str(len(data)) + ", " + hash_initializer(data) + "}"


parser = argparse.ArgumentParser()
parser.add_argument("--stock", type=Path, required=True)
parser.add_argument("--hoc", type=Path, required=True)
parser.add_argument("--hoc-elf", type=Path, required=True)
parser.add_argument("--nm", required=True)
parser.add_argument("--mesosphere", type=Path, required=True)
parser.add_argument("--atmosphere-zip", type=Path, required=True)
parser.add_argument("--hoc-zip", type=Path, required=True)
parser.add_argument("--atmosphere-license", type=Path, required=True)
parser.add_argument("--hoc-license", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()

archive = args.atmosphere_zip.read_bytes()
if digest(archive) != ATMOSPHERE_ZIP_SHA256:
    raise SystemExit("Atmosphere 1.11.2 release archive hash mismatch")
with zipfile.ZipFile(args.atmosphere_zip) as z:
    package3 = z.read("atmosphere/package3")
if digest(package3) != PACKAGE3_SHA256:
    raise SystemExit("Atmosphere 1.11.2 package3 hash mismatch")
hoc_archive = args.hoc_zip.read_bytes()
if digest(hoc_archive) != HOC_ZIP_SHA256:
    raise SystemExit("HOC 2.5.1 release archive hash mismatch")
with zipfile.ZipFile(args.hoc_zip) as z:
    released_hoc = z.read("atmosphere/kips/hoc.kip")

files = {
    "loader-stock.kip": args.stock,
    "loader-hoc.kip": args.hoc,
    "mesosphere.bin": args.mesosphere,
}
data = {name: path.read_bytes() for name, path in files.items()}
kip_loader(data["loader-stock.kip"])
kip_loader(data["loader-hoc.kip"])
hoc = data["loader-hoc.kip"]
offsets = [match.start() for match in re.finditer(b"CUST", hoc[256:])]
if len(offsets) != 1:
    raise SystemExit("HOC loader needs one uncompressed CUST table")
offset = offsets[0] + 256
revision = int.from_bytes(hoc[offset + 4:offset + 8], "little")
version = int.from_bytes(hoc[offset + 8:offset + 12], "little")
if not 0 < revision < 256 or not 0 < version < 10000:
    raise SystemExit("Invalid HOC CUST header")
symbol = subprocess.check_output([args.nm, "-S", "--demangle", str(args.hoc_elf)], text=True)
matches = re.findall(r"^[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+\S\s+ams::ldr::hoc::C$", symbol, re.M)
if len(matches) != 1:
    raise SystemExit("Could not find the HOC CUST symbol size")
config_size = int(matches[0], 16)
if config_size < 12 or config_size > len(hoc) - offset:
    raise SystemExit("HOC CUST symbol does not fit in the KIP")
released_offset = released_hoc.find(b"CUST", 256)
if released_offset < 0 or released_hoc.find(b"CUST", released_offset + 4) >= 0 or \
        released_hoc[released_offset:released_offset + config_size] != hoc[offset:offset + config_size]:
    raise SystemExit("Patched HOC settings layout differs from the 2.5.1 release")
hoc_code = bytearray(hoc)
hoc_code[offset + 12:offset + config_size] = bytes(config_size - 12)

romfs = args.output / "romfs" / "setup"
romfs.mkdir(parents=True, exist_ok=True)
(romfs / "exosphere-hoc.bin").unlink(missing_ok=True)
for name, source in files.items():
    shutil.copyfile(source, romfs / name)
shutil.copyfile(args.atmosphere_license, romfs / "LICENSE.Atmosphere")
shutil.copyfile(args.hoc_license, romfs / "LICENSE.HOC")

header = """#ifndef WINE_NX_SETUP_BOOT_MANIFEST_H
#define WINE_NX_SETUP_BOOT_MANIFEST_H
static const struct setup_boot_manifest setup_boot_release_manifest = {
    .version = 2,
    .release = "ams-1.11.2-hoc-2.5.1",
    .atmosphere_version = UINT32_C(0x010b02),
"""
for field, name in (("stock", "loader-stock.kip"), ("hoc", "loader-hoc.kip"),
                    ("mesosphere", "mesosphere.bin")):
    header += f"    .{field} = {payload(data[name])},\n"
header += f"    .hoc_code_sha256 = {hash_initializer(hoc_code)},\n"
header += f"    .hoc_config_offset = {offset},\n"
header += f"    .hoc_config_size = {config_size},\n"
header += f"    .hoc_config_revision = {revision},\n"
header += f"    .hoc_kip_version = {version},\n"
header += "};\n#endif\n"
(args.output / "setup_boot_manifest.h").write_text(header)
print(f"Boot bundle: Atmosphere 1.11.2, HOC 2.5.1 KIP version {version}, CUST revision {revision}")
print(args.output)
