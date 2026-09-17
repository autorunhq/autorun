#!/usr/bin/env python3
"""Static 16:9 / 1280x720 patch for SPEED2.EXE (Black Box, Oct 29 2004).

Ports ThirteenAG NFSUnderground2.WidescreenFix math onto this exe. The ASI
cannot load here: Wine-NX must keep its own dinput8, and this build is not the
Feb 9 2005 binary the ASI targets.

Does not touch DirectInput, XInput, or keys.txt.
"""
from __future__ import annotations

import argparse
import shutil
import struct
import sys
from pathlib import Path

# --- ThirteenAG formulas (Frontend.ixx + includes/stdafx.cpp), Width=1280 Height=720 ---
# fHudScaleX = (1/Width * (Height/480)) * 2
# fHudPosX   = 640 / (640 * fHudScaleX)          -> 426.667  (replaces 320 origin)
# fHudPosX*2                                      -> 853.333  (replaces 640)
# CalculateWidescreenOffset = 320 - 240*(W/H)     -> -106.667
# fWidescreenHudOffset = -that                    -> +106.667
# At 16:9 the .dat X offsets apply as-is: left -109, right +119.
WIDTH, HEIGHT = 1280, 720
HUD_SCALE_43 = 0.003125  # 2/640
HUD_SCALE_169 = (1.0 / WIDTH * (HEIGHT / 480.0)) * 2.0  # 0.00234375
HUD_POS_43 = 320.0
HUD_POS_169 = 640.0 / (640.0 * HUD_SCALE_169)  # 426.666...
HUD_POS_X2_43 = 640.0
HUD_POS_X2_169 = HUD_POS_169 * 2.0  # 853.333...
FMV_HALF = 0.5
FMV_169 = 0.5 / ((4.0 / 3.0) / (16.0 / 9.0))  # 2/3

HUD_640 = struct.pack("<f", HUD_SCALE_43)
HUD_720 = struct.pack("<f", HUD_SCALE_169)
F_POS43 = struct.pack("<f", HUD_POS_43)
F_POS169 = struct.pack("<f", HUD_POS_169)
F_X2_43 = struct.pack("<f", HUD_POS_X2_43)
F_X2_169 = struct.pack("<f", HUD_POS_X2_169)

# Immediate 320.0 sites ThirteenAG writes (same byte patterns on this exe).
HUD_POS_IMM = (
    0x8B5D0,
    0x10B435,
    0x11B32B,
    0x136828,
    0x1369F9,
    0x136C29,
    0x136F71,
)
# .rdata globals: fsub 320.0 (24 uses) and fmul 640.0 (fHudPosX_x2).
HUD_POS_RDATA = 0x397D50
HUD_POS_X2_RDATA = 0x397D58
HUD_SCALE_OFF = 0x39AC0C

# Only the FMV quad ThirteenAG .count(1) hits. The other two ±0.5 push-quads are FE.
FMV_SITE = 0x13697E
FMV_FE_SITES = (0x136BA2, 0x136E8D)
FMV43 = bytes.fromhex("680000003F680000003F68000000BF68000000BF")
FMV169 = (
    bytes.fromhex("68")
    + struct.pack("<f", FMV_169)
    + bytes.fromhex("68")
    + struct.pack("<f", FMV_169)
    + bytes.fromhex("68")
    + struct.pack("<f", -FMV_169)
    + bytes.fromhex("68")
    + struct.pack("<f", -FMV_169)
)
FMV_TAIL = bytes.fromhex("8BCBE88737FEFF8B4424188BCB")

# GetRacingResolution jump-table bodies (file offsets in this build).
RES_SLOTS = (
    (0x1BF57D, bytes.fromhex("C70080020000C701E0010000"), bytes.fromhex("C70000050000C701D0020000")),
    (0x1BF594, bytes.fromhex("C70220030000C70058020000"), bytes.fromhex("C70200050000C700D0020000")),
    (0x1BF5AB, bytes.fromhex("C70100040000C70200030000"), bytes.fromhex("C70100050000C702D0020000")),
    (0x1BF5C2, bytes.fromhex("C70000050000C701C0030000"), bytes.fromhex("C70000050000C701D0020000")),
    (0x1BF5D9, bytes.fromhex("C70200050000C70000040000"), bytes.fromhex("C70200050000C700D0020000")),
    (0x1BF5F0, bytes.fromhex("C70140060000C702B0040000"), bytes.fromhex("C70100050000C702D0020000")),
)

# Radar mask (ThirteenAG: width*2 / height*2). File offsets of the float immediates.
RADAR_W = (0x1C71BA, 0x1C71CA)
RADAR_H = (0x1C71D2, 0x1C71E2)
RADAR_W_43, RADAR_H_43 = 640.0, 480.0
RADAR_W_169, RADAR_H_169 = float(WIDTH * 2), float(HEIGHT * 2)

# Rear-view mirror X (ThirteenAG: (fHudPosX-320)+200 / +440).
MIRROR_LEFT = (0x1CC059, 0x1CC069)  # 200.0
MIRROR_RIGHT = (0x1CC049, 0x1CC079)  # 440.0
MIRROR_L_43, MIRROR_R_43 = 200.0, 440.0
MIRROR_L_169 = (HUD_POS_169 - 320.0) + 200.0
MIRROR_R_169 = (HUD_POS_169 - 320.0) + 440.0

# HUD draw hooks (same patterns as ThirteenAG 1.1 / this Oct 29 2004 exe).
HUDHOOK_OFF = 0x11B0F0
HUDHOOK_ORIG = bytes.fromhex("894C246089542464")
BLIPS_OFF = 0x11D5B5  # +7 into "8B 4B 1C 8B 54 24 18 89 0A 8B 43 20"
BLIPS_ORIG = bytes.fromhex("890A8B4320")
HUDHOOK2_OFF = 0xC66B3
HUDHOOK2_ORIG = bytes.fromhex("D956488B4E1C")

CAVE_OFF = 0x382A34  # .text slack, 4-aligned for x87 dword accesses
CAVE_VA = 0x400000 + CAVE_OFF
IMAGE_BASE = 0x400000
CAVE_MAGIC = b"NXH1"

BUILD_DATE = b"Oct 29 2004"

# ThirteenAG NFSUnderground2.WidescreenFix.dat (in-race HUD widgets only).
# At 1280x720 the applied X delta equals OffsetX.
HUD_DAT = """
-295 -170 -109
-295 -186 -109
-295 -204 -109
-295 -211 -109
-173 -177 -109
-127 -177 -109
-275 -181 -109
-125 -177 -109
-31 -177 -109
-131 -177 -109
-42 -177 -109
-275 -182 -109
-43 -177 -109
-92 -177 -109
-93 -177 -109
33 -177 -109
-223 70 -109
-223 142 -109
-223 71 -109
-223 143 -109
-143 196 -109
-248 -122 -109
-195 -112 -109
-185 -66 -109
-196 -185 -109
-224 -117 -109
142 195 119
199 177 119
-110 -26 -109
-254 171 -109
205 181 119
168 202 119
207 175 119
160 206 119
220 151 119
253 117 119
-263 36 -109
285 14 119
270 91 119
284 -9 119
284 -2 119
295 -89 119
295 -78 119
-246 -158 -109
224 -157 119
257 -182 119
213 -224 119
196 -100 119
215 -159 119
224 -52 119
253 -149 119
217 -161 119
240 -161 119
139 -163 119
223 -52 119
266 -180 119
280 -129 119
275 -167 119
187 -190 119
179 -134 119
242 -161 119
219 -161 119
140 -162 119
223 -52 119
224 -132 119
191 -127 119
201 -137 119
227 -184 119
193 -194 119
283 -163 119
-223 34 -109
-223 -87 -109
-223 -172 -109
"""


def _i8(n: int) -> bytes:
    return bytes([n & 0xFF])


def _i32(n: int) -> bytes:
    return struct.pack("<i", n)


def _u32(n: int) -> bytes:
    return struct.pack("<I", n)


def hud_table_bytes() -> bytes:
    seen: set[tuple[int, int]] = set()
    out = bytearray()
    for line in HUD_DAT.strip().splitlines():
        x, y, dx = (int(float(p)) for p in line.split())
        key = (x, y)
        if key in seen:
            continue
        seen.add(key)
        out += struct.pack("<hhf", x, y, float(dx))
    return bytes(out)


class Cave:
    """Assemble the WidescreenHud lookup + trampolines into .text slack."""

    def __init__(self, va: int):
        self.va = va
        self.buf = bytearray()
        self.labels: dict[str, int] = {}

    def mark(self, name: str) -> None:
        self.labels[name] = len(self.buf)

    def emit(self, data: bytes) -> None:
        self.buf += data

    def loc(self, name: str) -> int:
        return self.va + self.labels[name]

    def rel32_to(self, target_va: int) -> bytes:
        # caller emits opcode first; rel is from the end of the disp32
        raise AssertionError("use emit_rel32")

    def emit_jmp(self, target_va: int) -> None:
        rel = target_va - (self.va + len(self.buf) + 5)
        self.emit(b"\xE9" + _i32(rel))

    def emit_call(self, target_va: int) -> None:
        rel = target_va - (self.va + len(self.buf) + 5)
        self.emit(b"\xE8" + _i32(rel))

    def emit_jcc(self, op: bytes, label: str, fixups: list) -> None:
        self.emit(op)
        fixups.append((len(self.buf), label, len(op)))
        self.emit(b"\x00\x00\x00\x00" if len(op) == 2 else b"\x00")

    def patch_rel(self, fixups: list) -> None:
        for off, label, oplen in fixups:
            target = self.loc(label)
            inst_end = self.va + off + (4 if oplen == 2 else 1)
            rel = target - inst_end
            if oplen == 2:
                struct.pack_into("<i", self.buf, off, rel)
            else:
                if not -128 <= rel <= 127:
                    raise SystemExit(f"short jump to {label} out of range ({rel})")
                self.buf[off] = rel & 0xFF


def build_cave() -> bytes:
    table = hud_table_bytes()
    c = Cave(CAVE_VA)
    c.emit(CAVE_MAGIC)  # 0
    c.mark("tmp")
    c.emit(b"\x00\x00\x00\x00")  # 4
    c.mark("cw")
    c.emit(b"\x00\x00")  # 8 saved CW
    c.mark("chop")
    c.emit(b"\x00\x00")  # 10
    c.emit(b"\x00\x00\x00\x00")  # 12 pad so table is 16-byte / 4-aligned
    c.mark("table")
    c.emit(table)
    c.mark("table_end")

    va_tmp, va_cw, va_chop = c.loc("tmp"), c.loc("cw"), c.loc("chop")
    va_table, va_table_end = c.loc("table"), c.loc("table_end")

    c.mark("adjust")
    # ecx=x bits, edx=y bits in/out. FPU stack balanced. Restores x87 CW.
    c.emit(b"\x50\x53\x56")  # push eax, ebx, esi
    c.emit(b"\xD9\x3D" + _u32(va_cw))  # fnstcw [cw]
    c.emit(b"\x66\xA1" + _u32(va_cw))  # mov ax, [cw]
    c.emit(b"\x66\x0D\x00\x0C")  # or ax, 0x0C00  (RC=chop)
    c.emit(b"\x66\xA3" + _u32(va_chop))  # mov [chop], ax
    c.emit(b"\xD9\x2D" + _u32(va_chop))  # fldcw [chop]
    c.emit(b"\x89\x0D" + _u32(va_tmp))  # mov [tmp], ecx
    c.emit(b"\xD9\x05" + _u32(va_tmp))  # fld dword [tmp]
    c.emit(b"\xDB\x1D" + _u32(va_tmp))  # fistp dword [tmp]
    c.emit(b"\xA1" + _u32(va_tmp))  # mov eax, [tmp]  ; xi
    c.emit(b"\x89\x15" + _u32(va_tmp))  # mov [tmp], edx
    c.emit(b"\xD9\x05" + _u32(va_tmp))
    c.emit(b"\xDB\x1D" + _u32(va_tmp))
    c.emit(b"\x8B\x1D" + _u32(va_tmp))  # mov ebx, [tmp]  ; yi
    c.emit(b"\xD9\x2D" + _u32(va_cw))  # fldcw [saved]
    c.emit(b"\xBE" + _u32(va_table))  # mov esi, table

    c.mark("search")
    fixups: list = []
    c.emit(b"\x81\xFE" + _u32(va_table_end))  # cmp esi, table_end
    c.emit_jcc(b"\x0F\x83", "done", fixups)  # jae done
    c.emit(b"\x66\x3B\x06")  # cmp ax, [esi]
    c.emit_jcc(b"\x75", "next", fixups)  # jne next
    c.emit(b"\x66\x3B\x5E\x02")  # cmp bx, [esi+2]
    c.emit_jcc(b"\x75", "next", fixups)
    c.emit(b"\x89\x0D" + _u32(va_tmp))  # mov [tmp], ecx
    c.emit(b"\xD9\x05" + _u32(va_tmp))
    c.emit(b"\xD8\x46\x04")  # fadd dword [esi+4]
    c.emit(b"\xD9\x1D" + _u32(va_tmp))
    c.emit(b"\x8B\x0D" + _u32(va_tmp))  # mov ecx, [tmp]
    c.emit_jcc(b"\xEB", "done", fixups)
    c.mark("next")
    c.emit(b"\x83\xC6\x08")  # add esi, 8
    c.emit_jcc(b"\xEB", "search", fixups)
    c.mark("done")
    c.emit(b"\x5E\x5B\x58\xC3")  # pop esi, ebx, eax; ret
    c.patch_rel(fixups)

    va_adjust = c.loc("adjust")

    c.mark("tramp_hud")
    c.emit(b"\x9C")  # pushfd  (following jz uses prior flags)
    c.emit_call(va_adjust)
    c.emit(b"\x89\x4C\x24\x64")  # mov [esp+64h], ecx
    c.emit(b"\x89\x54\x24\x68")  # mov [esp+68h], edx
    c.emit(b"\x9D")  # popfd
    c.emit_jmp(IMAGE_BASE + HUDHOOK_OFF + 8)

    c.mark("tramp_blips")
    c.emit(b"\x52")  # push edx  (dest pointer)
    c.emit(b"\x8B\x4B\x1C")  # mov ecx, [ebx+1Ch]
    c.emit(b"\x8B\x53\x20")  # mov edx, [ebx+20h]
    c.emit_call(va_adjust)
    c.emit(b"\x89\x4B\x1C")
    c.emit(b"\x89\x53\x20")
    c.emit(b"\x5A")  # pop edx
    c.emit(b"\x89\x0A")  # mov [edx], ecx
    c.emit(b"\x8B\x43\x20")  # mov eax, [ebx+20h]
    c.emit_jmp(IMAGE_BASE + BLIPS_OFF + 5)

    c.mark("tramp_hud2")
    c.emit(b"\xD9\x56\x48")  # fst dword [esi+48h]
    c.emit(b"\x8B\x4E\x1C")
    c.emit(b"\x8B\x56\x20")
    c.emit_call(va_adjust)
    c.emit_jmp(IMAGE_BASE + HUDHOOK2_OFF + 6)

    c.labels["tramp_hud"] = c.labels["tramp_hud"]  # keep
    cave = bytes(c.buf)
    # Stash tramp VAs in a sidecar the caller reads via rebuild... return buf + dict
    Cave.last_labels = {k: c.va + v for k, v in c.labels.items()}  # type: ignore[attr-defined]
    Cave.last_size = len(cave)  # type: ignore[attr-defined]
    if len(cave) > 0x5CF:
        raise SystemExit(f"HUD cave too large: {len(cave)} > 1487")
    return cave


def put_float(buf: bytearray, off: int, expect: tuple[float, ...], new: float, name: str) -> str:
    got = struct.unpack_from("<f", buf, off)[0]
    if any(abs(got - e) < 1e-4 for e in expect) or abs(got - new) < 1e-4:
        buf[off : off + 4] = struct.pack("<f", new)
        return f"{name}@0x{off:x} {got:.4f}->{new:.4f}"
    raise SystemExit(f"{name} at 0x{off:x}: unexpected {got!r} ({buf[off:off+4].hex()})")


def write_jmp5(buf: bytearray, src_off: int, target_va: int, orig: bytes) -> None:
    cur = bytes(buf[src_off : src_off + len(orig)])
    if cur != orig and cur[:1] != b"\xE9":
        raise SystemExit(f"hook at 0x{src_off:x}: expected {orig.hex()} or jmp, got {cur.hex()}")
    rel = target_va - (IMAGE_BASE + src_off + 5)
    patch = b"\xE9" + _i32(rel)
    patch = patch + b"\x90" * (len(orig) - 5)
    buf[src_off : src_off + len(orig)] = patch


def bump_text_vsz(buf: bytearray, new_vsz: int) -> None:
    e_lfanew = struct.unpack_from("<I", buf, 0x3C)[0]
    optsz = struct.unpack_from("<H", buf, e_lfanew + 4 + 16)[0]
    sect = e_lfanew + 4 + 20 + optsz  # first section = .text
    name = bytes(buf[sect : sect + 5])
    if name != b".text":
        raise SystemExit(f"first section is {name!r}, not .text")
    struct.pack_into("<I", buf, sect + 8, new_vsz)


def patch(data: bytes) -> bytearray:
    if BUILD_DATE not in data:
        raise SystemExit("not the Oct 29 2004 SPEED2.EXE this patch was reversed from")
    if len(data) != 4788224:
        raise SystemExit(f"unexpected size {len(data)} (want 4788224)")
    buf = bytearray(data)

    log: list[str] = []

    for off, old, new in RES_SLOTS:
        got = bytes(buf[off : off + len(new)])
        if got == new:
            continue
        if got != old:
            raise SystemExit(f"resolution slot at 0x{off:x}: expected {old.hex()} or patched, got {got.hex()}")
        buf[off : off + len(new)] = new
        log.append(f"res@0x{off:x}")

    log.append(put_float(buf, HUD_SCALE_OFF, (HUD_SCALE_43,), HUD_SCALE_169, "fHudScaleX"))

    for off in HUD_POS_IMM:
        log.append(put_float(buf, off, (HUD_POS_43,), HUD_POS_169, "fHudPosX"))

    log.append(put_float(buf, HUD_POS_RDATA, (HUD_POS_43,), HUD_POS_169, "fHudPosX_rdata"))
    log.append(put_float(buf, HUD_POS_X2_RDATA, (HUD_POS_X2_43,), HUD_POS_X2_169, "fHudPosX_x2"))

    # FMV: keep only the real movie quad; restore FE quads if a previous patch hit them.
    if bytes(buf[FMV_SITE : FMV_SITE + 20]) not in (FMV43, FMV169):
        raise SystemExit(f"FMV at 0x{FMV_SITE:x} mismatch")
    if bytes(buf[FMV_SITE + 20 : FMV_SITE + 20 + len(FMV_TAIL)]) != FMV_TAIL:
        raise SystemExit("FMV tail mismatch; refusing to patch the wrong quad")
    buf[FMV_SITE : FMV_SITE + 20] = FMV169
    log.append("fmv@0x13697e 16:9")
    for off in FMV_FE_SITES:
        got = bytes(buf[off : off + 20])
        if got not in (FMV43, FMV169):
            raise SystemExit(f"FE quad at 0x{off:x}: unexpected {got.hex()}")
        buf[off : off + 20] = FMV43
        log.append(f"fe-quad@0x{off:x} restored 4:3")

    for off in RADAR_W:
        log.append(put_float(buf, off, (RADAR_W_43,), RADAR_W_169, "radarW"))
    for off in RADAR_H:
        log.append(put_float(buf, off, (RADAR_H_43,), RADAR_H_169, "radarH"))
    for off in MIRROR_LEFT:
        log.append(put_float(buf, off, (MIRROR_L_43,), MIRROR_L_169, "mirrorL"))
    for off in MIRROR_RIGHT:
        log.append(put_float(buf, off, (MIRROR_R_43,), MIRROR_R_169, "mirrorR"))

    slack_start = 0x382A31
    slack = bytes(buf[slack_start:0x383000])
    if CAVE_MAGIC not in slack[:16] and any(slack):
        raise SystemExit(f".text slack busy: {slack[:16].hex()}")
    buf[slack_start:0x383000] = b"\x00" * (0x383000 - slack_start)

    cave = build_cave()
    labels = Cave.last_labels  # type: ignore[attr-defined]
    buf[CAVE_OFF : CAVE_OFF + len(cave)] = cave
    if CAVE_OFF + len(cave) > 0x383000:
        raise SystemExit("cave overruns .text")
    bump_text_vsz(buf, (CAVE_OFF + len(cave)) - 0x1000)

    write_jmp5(buf, HUDHOOK_OFF, labels["tramp_hud"], HUDHOOK_ORIG)
    write_jmp5(buf, BLIPS_OFF, labels["tramp_blips"], BLIPS_ORIG)
    write_jmp5(buf, HUDHOOK2_OFF, labels["tramp_hud2"], HUDHOOK2_ORIG)
    log.append(f"hud-cave {len(cave)} bytes @0x{CAVE_OFF:x}")

    print("patch:", "; ".join(log))
    return buf


def verify(buf: bytes) -> None:
    assert buf[HUD_SCALE_OFF : HUD_SCALE_OFF + 4] == HUD_720
    for off in HUD_POS_IMM:
        assert abs(struct.unpack_from("<f", buf, off)[0] - HUD_POS_169) < 1e-4
    assert abs(struct.unpack_from("<f", buf, HUD_POS_RDATA)[0] - HUD_POS_169) < 1e-4
    assert abs(struct.unpack_from("<f", buf, HUD_POS_X2_RDATA)[0] - HUD_POS_X2_169) < 1e-4
    assert buf[FMV_SITE : FMV_SITE + 20] == FMV169
    for off in FMV_FE_SITES:
        assert buf[off : off + 20] == FMV43, hex(off)
    assert buf[CAVE_OFF : CAVE_OFF + 4] == CAVE_MAGIC
    assert buf[HUDHOOK_OFF] == 0xE9
    assert buf[BLIPS_OFF] == 0xE9
    assert buf[HUDHOOK2_OFF] == 0xE9
    assert BUILD_DATE in buf


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("exe", type=Path)
    ap.add_argument("-o", "--output", type=Path)
    ap.add_argument("--in-place", action="store_true")
    args = ap.parse_args()
    raw = args.exe.read_bytes()
    out = patch(raw)
    verify(out)
    dest = args.exe if args.in_place else args.output
    if dest is None:
        dest = args.exe.with_name(args.exe.stem + ".ws720" + args.exe.suffix)
    if dest.resolve() == args.exe.resolve() and not args.in_place:
        raise SystemExit("refusing to overwrite without --in-place")
    if args.in_place:
        bak = args.exe.with_suffix(args.exe.suffix + ".bak-4x3")
        if not bak.exists():
            shutil.copy2(args.exe, bak)
    dest.write_bytes(out)
    print(f"wrote {dest} ({len(out)} bytes), 1280x720 16:9 HUD")
    return 0


if __name__ == "__main__":
    sys.exit(main())
