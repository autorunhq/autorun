#!/usr/bin/env python3
"""Generate Autorun's carafe-menu (version 2) catalog from Autorun's LGPL sources.

Copyright: LGPL-2.1-or-later, matching the Autorun runtime files it reads.

The settings, their order, their values and the English texts are Autorun's.
The other fourteen languages are Carafe's texts for the same setting, read
from carafe-menu-texts.json beside this file (GPL-3.0-or-later, as that file
says; carafe/integration/tools/menu_texts.py writes it from a checkout of
Carafe). The catalog this writes therefore carries GPL-3.0-or-later texts.
What has no text in Carafe stays English in every language.
"""
import argparse
import json
import re
from pathlib import Path

LANGUAGES = [
    "ru", "en", "zh-Hans", "ja", "ko", "zh-Hant",
    "fr", "fr-CA", "de", "it", "es", "es-419", "nl", "pt", "pt-BR",
]


def t15(en: str, overrides=None):
    overrides = overrides or {}
    clean_en = en.replace("\t", " ").replace("\n", " ").strip()
    out = []
    for code in LANGUAGES:
        if code in overrides:
            out.append(overrides[code].replace("\t", " ").replace("\n", " ").strip())
        elif code == "en":
            out.append(clean_en)
        else:
            out.append("")
    return "\t".join(out)


# The row that writes nothing is, in Carafe's catalog too, a row that writes
# nothing (DXVK's own default, no frame limit).
ABSENT = object()

# Settings Carafe has a text for that is not true here; they stay English.
NOT_CARAFES = {
    # Carafe's hint says what "Default" does in Carafe's DXVK build (async).
    # Here the option is DXVK-Sarek's too, where the default is dyasync.
    "dxvk.shaderCompilationMethod",
}

# Autorun's pages and the Carafe pages most of their settings are on there.
CARAFE_PAGES = {
    "memory": ["dxvk-memory"],
    "frames": ["dxvk-frames"],
    "shaders": ["dxvk-shaders", "dxvk-quality"],
    "display": ["dxvk-display"],
    "compat": ["dxvk-fixes"],
    "basic": ["fex-general"],
    "precision": ["fex-memory"],
    "cache": ["fex-compat"],
}


class CarafeTexts:
    """Carafe's texts by what a setting is here: its file, its key and a value's stored text."""

    def __init__(self, path: Path):
        self.data = json.loads(path.read_text(encoding="utf-8"))
        if self.data["languages"] != LANGUAGES:
            raise SystemExit(f"{path.name} is not in this catalog's languages")
        self.texts = self.data["texts"]
        self.used = set()
        self.ids = {}

    def english(self, key: str) -> str:
        return self.texts[key][LANGUAGES.index("en")]

    def of(self, label):
        """The fourteen texts of a label: a key, or a key and the number that goes into it."""
        if label is None:
            return None
        key, number = (label, None) if isinstance(label, str) else label
        self.used.add(key)
        return {
            code: text if number is None else text.replace("{{value}}", number)
            for code, text in zip(LANGUAGES, self.texts[key]) if code != "en"
        }

    def same(self, en: str, key: str):
        """Carafe's other languages for a text whose English is word for word Autorun's."""
        if self.english(key) != en:
            raise SystemExit(f"Carafe's {key} is {self.english(key)!r}, not {en!r}")
        return self.of(key)

    def joined(self, keys):
        parts = [self.of(key) for key in keys]
        return {code: " & ".join(part[code] for part in parts) for code in parts[0]}

    def setting(self, file_name: str, key: str):
        return None if key in NOT_CARAFES else self.data["settings"].get(f"{file_name} {key}")

    def value(self, entry, stored: str, unset):
        """A value row's texts. `unset` says what the row that writes nothing stands for."""
        if entry is None:
            return None
        if stored != "-":
            return self.of(entry["values"].get(stored))
        if unset is ABSENT:
            # Carafe's word alone. English also says what the default is
            # ("Default (False / Auto)"); with that added, the longest rows of
            # four languages no longer fit beside the setting's name.
            return self.of(entry.get("absent"))
        return self.of(entry["values"].get(unset)) if unset is not None else None


CARAFE = None


def clean_id(prefix: str, key: str) -> str:
    slug = re.sub(r"[^a-zA-Z0-9]+", "-", key).strip("-").lower()
    return f"{prefix}-{slug}" if prefix else slug


def parse_dxvk_options(dxvk_c: Path):
    text = dxvk_c.read_text(encoding="utf-8")

    # Parse static const struct dxvk_option_choice arrays.
    choice_arrays = {}
    arr_re = re.compile(
        r"static\s+const\s+struct\s+dxvk_option_choice\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};",
        re.S,
    )
    pair_re = re.compile(r'\{\s*(NULL|"(?:[^"\\]|\\.)*")\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}')
    for m in arr_re.finditer(text):
        name, body = m.group(1), m.group(2)
        items = []
        for pm in pair_re.finditer(body):
            raw_val, label = pm.group(1), pm.group(2)
            val = "-" if raw_val == "NULL" else bytes(raw_val[1:-1], "utf-8").decode("unicode_escape")
            items.append((val, label))
        choice_arrays[name] = items

    # Parse OPTION(...) entries inside dxvk_options[].
    opt_re = re.compile(
        r'OPTION\(\s*"([^"]+)"\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+),\s*(\w+)\s*,'
        r'\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*"([^"]*)"\s*\)',
        re.S,
    )
    options_by_key = {}
    order = []
    for m in opt_re.finditer(text):
        key = m.group(1)
        help_text = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2)))
        help_text = bytes(help_text, "utf-8").decode("unicode_escape")
        arr_name = m.group(3)
        choices = list(choice_arrays[arr_name])
        default_note = m.group(10)
        # First entry is Default ("-"); annotate default value cleanly.
        if choices and choices[0][0] == "-":
            choices[0] = ("-", f"Default ({default_note})")
        if key in options_by_key:
            # Merge choices cleanly (e.g. d3d9.floatEmulation & d3d9.deviceLocalConstantBuffers).
            existing = options_by_key[key]
            # One row serves every DXVK build that reads the option; where
            # their defaults differ it names each (dxvk.shaderCompilationMethod).
            if default_note not in existing["defaults"]:
                existing["defaults"].append(default_note)
                existing["choices"][0] = ("-", "Default (%s)" % " / ".join(existing["defaults"]))
            seen_vals = {v for v, _ in existing["choices"]}
            for val, lbl in choices:
                if val not in seen_vals:
                    existing["choices"].append((val, lbl))
                    seen_vals.add(val)
            if len(help_text) > len(existing["help"]):
                existing["help"] = help_text
        else:
            options_by_key[key] = {
                "key": key,
                "help": help_text,
                "choices": choices,
                "defaults": [default_note],
            }
            order.append(key)
    return [options_by_key[k] for k in order]


def parse_fex_options(fex_h: Path):
    text = fex_h.read_text(encoding="utf-8")
    # Match entries inside nx_fex_options[].
    entry_re = re.compile(
        r'\{\s*(NX_FEX_\w+)\s*,\s*"(FEX_[^"]+)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,'
        r'\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*\{([^}]*)\}\s*,\s*\{([^}]*)\}\s*\}',
        re.S,
    )
    options = []
    for m in entry_re.finditer(text):
        macro_id = m.group(1)
        key = m.group(2)
        help_text = m.group(3)
        advanced = int(m.group(4))
        default_choice = int(m.group(5))
        vals = re.findall(r'"([^"]*)"', m.group(7))
        labels = re.findall(r'"([^"]*)"', m.group(8))
        options.append({
            "macro": macro_id,
            "key": key,
            "help": help_text,
            "advanced": advanced,
            "default_choice": default_choice,
            "choices": list(zip(vals, labels)),
        })
    return options


def parse_input_data(input_c: Path, key_names_h: Path):
    ic = input_c.read_text(encoding="utf-8")
    m = re.search(r"input_pad_labels\s*\[\s*INPUT_PAD_TARGET_COUNT\s*\]\s*=\s*\{(.*?)\};", ic, re.S)
    pad_labels = re.findall(r'"([^"]+)"', m.group(1))
    assert len(pad_labels) == 24, f"Expected 24 pad labels, got {len(pad_labels)}"

    kh = key_names_h.read_text(encoding="utf-8")
    keys = []
    for km in re.finditer(r'\{\s*(0x[0-9a-fA-F]+)\s*,\s*"([^"]+)"\s*\}', kh):
        code = int(km.group(1), 16)
        if code == 0:
            continue
        keys.append((code, km.group(2)))

    # Extra modifier / extended keys + mouse actions recognized by input_key_label().
    extra_keys = [
        (0xa0, "Left Shift"),
        (0xa1, "Right Shift"),
        (0xa2, "Left Ctrl"),
        (0xa3, "Right Ctrl"),
        (0xa4, "Left Alt"),
        (0xa5, "Right Alt"),
        (0x5b, "Left Windows"),
        (0x5c, "Right Windows"),
        (0x5d, "Menu"),
        (0x90, "Num lock"),
        (0x91, "Scroll lock"),
        (0x10d, "Numpad Enter"),
        (0x100, "Left click"),
        (0x101, "Right click"),
        (0x102, "Middle click"),
        (0x103, "Mouse back"),
        (0x104, "Mouse forward"),
        (0x105, "Wheel up"),
        (0x106, "Wheel down"),
        (0x107, "Wheel left"),
        (0x108, "Wheel right"),
        (0x109, "Mouse up"),
        (0x10a, "Mouse down"),
        (0x10b, "Mouse left"),
        (0x10c, "Mouse right"),
    ]
    seen_codes = {c for c, _ in keys}
    for code, name in extra_keys:
        if code not in seen_codes:
            keys.append((code, name))
            seen_codes.add(code)

    return pad_labels, keys


def dxvk_category(key: str) -> str:
    if any(t in key for t in ("Memory", "memory", "Buffers", "buffers", "UMA", "Defrag")):
        return "memory"
    if any(t in key for t in ("Thread", "Threads", "Latency", "BackBuffers", "framePace", "tearFree", "Pipeline", "StateCache", "gplAsync")):
        return "frames"
    if any(t in key for t in ("Shader", "Compilation", "Dyasync", "Anisotropy", "LodBias", "Msaa", "FP16", "SinCos", "sincos", "float", "Float", "pow", "Pow", "TessFactor", "Sampler", "ImageLayout", "ImplicitResolves", "RawSsbo")):
        return "shaders"
    if any(t in key for t in ("Nvidia", "Nvk", "FeatureLevel", "AspectRatio", "RefreshRate", "modeCount", "deferSurface")):
        return "display"
    return "compat"


def dxvk_api(key: str) -> str:
    if key.startswith("d3d9."):
        return "d3d8,d3d9"
    if key.startswith("d3d11.") or key.startswith("dxgi."):
        return "d3d10,d3d11"
    return "-"


def dxvk_nice_name(key: str) -> str:
    return key


def fex_nice_name(key: str) -> str:
    mapping = {
        "FEX_TSOENABLED": "Memory ordering (TSO)",
        "FEX_MULTIBLOCK": "Multi-block compilation",
        "FEX_MAXINST": "Max instructions per block",
        "FEX_SMCCHECKS": "Self-modifying code checks",
        "FEX_DISABLEL2CACHE": "Disable L2 lookup cache",
        "FEX_DYNAMICL1CACHE": "Dynamic L1 cache",
        "FEX_X87REDUCEDPRECISION": "x87 64-bit reduced precision",
        "FEX_VECTORTSOENABLED": "Vector TSO",
        "FEX_MEMCPYSETTSOENABLED": "Memcpy/memset TSO",
        "FEX_HALFBARRIERTSOENABLED": "Half-barrier TSO atomics",
        "FEX_STRICTINPROCESSSPLITLOCKS": "Strict in-process split locks",
        "FEX_KERNELUNALIGNEDATOMICBACKPATCHING": "Unaligned atomic backpatching",
        "FEX_VOLATILEMETADATA": "Use PE volatile metadata",
        "FEX_MONOHACKS": "Mono SMC hacks",
        "FEX_HIDEHYPERVISORBIT": "Hide hypervisor bit",
        "FEX_SMALLTSCSCALE": "Small TSC scaling",
        "FEX_HIDEHYBRID": "Hide hybrid CPU topology",
        "FEX_DYNAMICL1CACHEINCREASECOUNTHEURISTIC": "Dynamic L1 grow threshold",
        "FEX_DYNAMICL1CACHEDECREASECOUNTHEURISTIC": "Dynamic L1 shrink threshold",
    }
    return mapping.get(key, key)


def emit_setting(lines, sid, file_name, key, apply_mode, default_idx, when, api, name, hint, values, unset=None):
    """`unset` is what the "-" row stands for: ABSENT, or the value a file would hold for it."""
    entry = CARAFE.setting(file_name, key)
    CARAFE.ids[sid] = f"{file_name} {key}"
    theirs = (CARAFE.of(entry["name"]), CARAFE.of(entry["hint"])) if entry else (None, None)
    lines.append(
        f"setting\t{sid}\t{file_name}\t{key}\t{apply_mode}\t{default_idx}\t{when}\t{api}"
        f"\t{t15(name, theirs[0])}\t{t15(hint, theirs[1])}"
    )
    for val, lbl in values:
        lines.append(f"value\t{val}\t{t15(lbl, CARAFE.value(entry, val.strip(chr(34)), unset))}")


def build_menu(horizon_wine: Path) -> str:
    source = horizon_wine / "source"
    dxvk_opts = parse_dxvk_options(source / "dxvk_options.c")
    fex_opts = parse_fex_options(source / "fex_options.h")
    pad_labels, key_items = parse_input_data(source / "input_profile.c", source / "key_names.h")

    global CARAFE
    CARAFE = CarafeTexts(Path(__file__).resolve().parent / "carafe-menu-texts.json")
    carafe = CARAFE.data

    lines = [
        "carafe-menu\t2",
        "languages\t" + "\t".join(LANGUAGES),
    ]

    # --- Section: Controls ---------------------------------------------------
    lines.append(f"section\tcontrols\t{t15('Controls', CARAFE.same('Controls', carafe['sections']['controls']))}")
    emit_setting(
        lines, "input-mode", "keys", "input-mode", "now", 0, "-", "-",
        "Input mode",
        "Controller presents an Xbox gamepad to the game. Keyboard / mouse sends keyboard keys and pointer motion.",
        [("1", "Controller"), ("2", "Keyboard / mouse")],
    )
    emit_setting(
        lines, "button-layout", "keys", "button-layout", "now", 0, "show:input-mode=0", "-",
        "Face button layout",
        "Matches the printed Xbox labels (B/A/Y/X) or swaps them to match Nintendo's positions (A/B/X/Y).",
        [("0", "Xbox (B / A / Y / X)"), ("1", "Nintendo (A / B / X / Y)")],
    )
    emit_setting(
        lines, "mouse-speed", "keys", "mouse-speed", "now", 3, "show:input-mode=1", "-",
        "Mouse speed",
        "Scales how fast the analog stick moves the pointer in keyboard & mouse mode.",
        [("25", "25%"), ("50", "50%"), ("75", "75%"), ("100", "100%"), ("150", "150%"), ("200", "200%"), ("400", "400%")],
    )
    emit_setting(
        lines, "left-deadzone", "keys", "left-deadzone", "now", 1, "-", "-",
        "Left stick deadzone",
        "Ignores small stick drift near center on the left analog stick.",
        [("0", "0%"), ("5", "5%"), ("10", "10%"), ("15", "15%"), ("20", "20%"), ("25", "25%")],
    )
    emit_setting(
        lines, "right-deadzone", "keys", "right-deadzone", "now", 1, "show:input-mode=0", "-",
        "Right stick deadzone",
        "Ignores small stick drift near center on the right analog stick.",
        [("0", "0%"), ("5", "5%"), ("10", "10%"), ("15", "15%"), ("20", "20%"), ("25", "25%")],
    )
    emit_setting(
        lines, "keyboard-auto", "keys", "keyboard-auto", "now", 1, "show:input-mode=1", "-",
        "On-screen keyboard",
        "Open the floating keyboard automatically when a Windows text box takes focus.",
        [("0", "Manual (- + B)"), ("1", "Automatic")],
    )

    # --- Section: Graphics ---------------------------------------------------
    lines.append(f"section\tgraphics\t{t15('Graphics', CARAFE.same('Graphics', carafe['sections']['graphics']))}")
    emit_setting(
        lines, "frame-limit", "program", "frame-limit", "now", 0, "-", "-",
        "Frame limit",
        "Caps frames per second to stabilize pacing and reduce power draw. Takes effect immediately.",
        [("-", "Off"), ("30", "30 fps"), ("40", "40 fps"), ("45", "45 fps"),
         ("60", "60 fps"), ("75", "75 fps"), ("90", "90 fps"), ("120", "120 fps")],
        unset=ABSENT,
    )
    emit_setting(
        lines, "vsync", "program", "vsync", "restart", 0, "-", "-",
        "Vertical sync",
        "Synchronizes Vulkan presentation with the display refresh to prevent tearing.",
        [("-", "Enabled"), ("0", "Disabled")],
        unset="1",
    )
    emit_setting(
        lines, "upscaling", "program", "upscaling", "restart", 0, "-", "-",
        "Upscaling",
        "Scales lower game resolutions to the Switch screen using bilinear filtering, AMD FSR 1.0, or integer scaling.",
        [("-", "Off (Bilinear)"), ("fsr", "FSR 1.0"), ("integer", "Integer")],
        unset="off",
    )
    emit_setting(
        lines, "upscaling-sharpness", "program", "upscaling-sharpness", "restart", 2, "on:upscaling=1", "-",
        "FSR sharpness",
        "Controls RCAS sharpening strength when FSR 1.0 upscaling is selected.",
        [("0%", "0%"), ("20%", "20%"), ("-", "40% (Default)"), ("60%", "60%"), ("80%", "80%"), ("100%", "100%")],
        unset="40%",
    )
    emit_setting(
        lines, "picture", "program", "picture", "restart", 0, "-", "-",
        "Picture fitting",
        "Controls how the game's window or aspect ratio fits the 16:9 display.",
        [("-", "Global setting"), ("whole", "Whole picture"), ("edges", "Fill screen"),
         ("stretched", "Stretched"), ("own-size", "Own size")],
    )
    emit_setting(
        lines, "windows", "program", "windows", "restart", 0, "-", "-",
        "Window presentation",
        "Selects the OpenGL compositor (supports FSR/overlays) or direct software framebuffer for GDI windows.",
        [("-", "Global setting"), ("compositor", "Compositor (OpenGL)"), ("framebuffer", "Framebuffer")],
    )
    emit_setting(
        lines, "dxvk-hud", "program", "dxvk-hud", "restart", 0, "show:d3d=0,2", "-",
        "DXVK HUD",
        "Displays live DXVK frame rate, GPU, and frame-time telemetry over the game.",
        [("-", "Disabled"), ("fps", "FPS"), ("api,fps,frametimes", "Compact"),
         ("version,api,devinfo,fps,memory,frametimes,compiler", "Full")],
        unset="0",
    )
    emit_setting(
        lines, "d3d", "program", "d3d", "restart", 0, "-", "-",
        "Direct3D backend",
        "Translates Direct3D 8-11 to Vulkan via DXVK, adds VKD3D-Proton for Direct3D 12, or uses WineD3D.",
        [("-", "DXVK"), ("wine", "Wine"), ("dxvk+vkd3d", "DXVK + VKD3D")],
    )
    emit_setting(
        lines, "dxvk-source", "program", "dxvk-source", "restart", 0, "on:d3d=0,2", "-",
        "DXVK build",
        "Chooses which DXVK build series is mapped into the prefix for this game.",
        [("-", "Official DXVK"), ("sarek", "DXVK Sarek"), ("gplasync", "DXVK Async")],
    )
    emit_setting(
        lines, "lsfg", "program", "lsfg", "restart", 0, "-", "-",
        "LSFG-VK frame generation",
        "Interpolates extra frames on Vulkan titles using LSFG-VK.",
        [("-", "Disabled"), ("1", "Enabled")],
        unset="0",
    )
    emit_setting(
        lines, "lsfg-performance", "program", "lsfg-performance", "restart", 0, "show:lsfg=1", "-",
        "LSFG performance mode",
        "Uses a faster optical-flow pass at lower GPU cost.",
        [("-", "Enabled"), ("0", "Disabled")],
        unset="1",
    )
    emit_setting(
        lines, "lsfg-flow", "program", "lsfg-flow", "restart", 1, "show:lsfg=1", "-",
        "LSFG flow scale",
        "Resolution scale for LSFG-VK motion estimation; lower values are faster.",
        [("0.125", "12.5%"), ("-", "25% (Default)"), ("0.5", "50%")],
        unset="0.25",
    )

    # Group: DXVK options (all 74 options from dxvk_options.c organized into 5 pages)
    lines.append(f"group\tdxvk\tshow:d3d=0,2\t{t15('DXVK options', CARAFE.same('DXVK options', carafe['groups']['dxvk']))}")
    pages = [
        ("memory", "Memory", []),
        ("frames", "Frames & latency", []),
        ("shaders", "Shaders & quality", []),
        ("display", "Display & GPU", []),
        ("compat", "Compatibility", []),
    ]
    page_map = {pid: lst for pid, _, lst in pages}
    for opt in dxvk_opts:
        page_map[dxvk_category(opt["key"])].append(opt)
    for pid, pname, opts in pages:
        if not opts:
            continue
        lines.append(f"page\t{pid}\t{t15(pname, CARAFE.joined(carafe['pages'][p] for p in CARAFE_PAGES[pid]))}")
        for opt in opts:
            emit_setting(
                lines,
                clean_id("dxvk", opt["key"]),
                "program",
                opt["key"],
                "restart",
                0,
                "-",
                dxvk_api(opt["key"]),
                dxvk_nice_name(opt["key"]),
                opt["help"],
                opt["choices"],
                unset=ABSENT,
            )
    lines.append("end")

    # --- Section: System -----------------------------------------------------
    lines.append(f"section\tsystem\t{t15('System', CARAFE.same('System', carafe['sections']['system']))}")
    emit_setting(
        lines, "cpu", "program", "cpu", "restart", 0, "-", "-",
        "CPU translator",
        "Uses FEX for x86/x86-64 emulation or Box64 as fallback.",
        [("-", "FEX"), ("box64", "Box64")],
    )
    emit_setting(
        lines, "four-cores", "program", "four-cores", "restart", 0, "-", "-",
        "Allow 4th CPU core",
        "Allows background/graphics workers to run across all four Horizon cores (3 program + 1 worker).",
        [("-", "Enabled"), ("0", "Disabled")],
    )
    emit_setting(
        lines, "sync", "program", "sync", "restart", 0, "-", "-",
        "Thread synchronization",
        "Standard uses Wine's synchronization; Horizon uses queued kernel waits which can cut overhead.",
        [("-", "Standard"), ("horizon", "Horizon queued waits")],
    )
    emit_setting(
        lines, "lazy-file-views", "program", "lazy-file-views", "restart", 0, "-", "-",
        "Lazy file views",
        "Reads file-backed memory views on demand instead of preloading entire mapped files from SD.",
        [("-", "Disabled"), ("1", "Enabled")],
    )
    emit_setting(
        lines, "verbose", "program", "verbose", "restart", 0, "-", "-",
        "Verbose logging",
        "Overrides global Wine trace logging for this game. Logging every operation slows performance.",
        [("-", "Global setting"), ("1", "Enabled"), ("0", "Disabled")],
    )
    emit_setting(
        lines, "profile", "program", "profile", "restart", 0, "-", "-",
        "Runtime profiler",
        "Logs thread, CPU, syscall, and frame profiling counters for diagnostics.",
        [("-", "Global setting"), ("1", "Enabled"), ("0", "Disabled")],
    )

    # Group: FEX options (all 19 options from fex_options.h)
    lines.append(f"group\tfex\tshow:cpu=0\t{t15('FEX options', CARAFE.same('FEX options', carafe['groups']['fex']))}")
    fex_pages = [
        ("basic", "Basic", []),
        ("precision", "Ordering & precision", []),
        ("cache", "Cache & topology", []),
    ]
    for opt in fex_opts:
        key = opt["key"]
        if not opt["advanced"]:
            fex_pages[0][2].append(opt)
        elif any(w in key for w in ("TSO", "LOCK", "ATOMIC", "VOLATILE")):
            fex_pages[1][2].append(opt)
        else:
            fex_pages[2][2].append(opt)
    for pid, pname, opts in fex_pages:
        lines.append(f"page\t{pid}\t{t15(pname, CARAFE.joined(carafe['pages'][p] for p in CARAFE_PAGES[pid]))}")
        for opt in opts:
            def_idx = opt["default_choice"]
            values = []
            for idx, (val, lbl) in enumerate(opt["choices"]):
                values.append(("-" if idx == def_idx else val, f"{lbl} (Default)" if idx == def_idx else lbl))
            emit_setting(
                lines,
                clean_id("fex", opt["key"]),
                "program",
                opt["key"],
                "restart",
                def_idx,
                "-",
                "-",
                fex_nice_name(opt["key"]),
                opt["help"],
                values,
                unset=opt["choices"][def_idx][0],
            )
    lines.append("end")

    # FEX Presets on System tab
    # The names are Carafe's words. So are two of the hints: those presets set
    # what Carafe's do. Carafe's Fast also turns the L1 cache's sizing off and
    # says so, which this one does not do; its hint stays English.
    presets = carafe["presets"]
    lines.append(
        f"preset\tbalanced\t{t15('Balanced (Default)', CARAFE.of(presets['balanced']['name']))}\t"
        f"{t15('Autorun defaults: TSO off, 1000-instruction blocks, dynamic L1 cache on.', CARAFE.of(presets['balanced']['hint']))}"
    )
    lines.append("set\tfex-fex-tsoenabled\t0")
    lines.append("set\tfex-fex-maxinst\t2")
    lines.append("set\tfex-fex-smcchecks\t0")
    lines.append("set\tfex-fex-x87reducedprecision\t1")

    lines.append(
        f"preset\treliable\t{t15('Reliable', CARAFE.same('Reliable', presets['reliable']['name']))}\t"
        f"{t15('Turns on TSO memory ordering, MTrack SMC checks, and full x87 precision for picky games.', CARAFE.of(presets['reliable']['hint']))}"
    )
    lines.append("set\tfex-fex-tsoenabled\t1")
    lines.append("set\tfex-fex-maxinst\t2")
    lines.append("set\tfex-fex-smcchecks\t1")
    lines.append("set\tfex-fex-x87reducedprecision\t0")

    lines.append(
        f"preset\tfast\t{t15('Fast', CARAFE.same('Fast', presets['fast']['name']))}\t"
        f"{t15('Larger 5000-instruction JIT blocks with TSO off for higher throughput.')}"
    )
    lines.append("set\tfex-fex-tsoenabled\t0")
    lines.append("set\tfex-fex-maxinst\t4")
    lines.append("set\tfex-fex-smcchecks\t0")
    lines.append("set\tfex-fex-x87reducedprecision\t1")

    # --- Section: Experimental -----------------------------------------------
    lines.append(f"section\texperimental\t{t15('Experimental', CARAFE.same('Experimental', carafe['sections']['experimental']))}")
    emit_setting(
        lines, "large-pages", "program", "large-pages", "restart", 0, "-", "-",
        "2 MB guest view alignment",
        "Aligns guest virtual memory views >= 2 MB on 2 MB boundaries to reduce TLB pressure.",
        [("-", "Disabled"), ("1", "Enabled")],
        unset="0",
    )

    # --- Error-screen suggestions ("What to try") ----------------------------
    suggestions = [
        ("crash", "fex-fex-tsoenabled", 1,
         "Enable x86 total store ordering if the crash looks like a multi-threading race."),
        ("crash", "fex-fex-smcchecks", 1,
         "Enable MTrack self-modifying code checks for JIT or DRM code."),
        ("crash", "fex-fex-x87reducedprecision", 0,
         "Restore full 80-bit x87 precision if physics or math fails."),
        ("crash", "d3d", 1,
         "Fall back to WineD3D if the crash happens inside DXVK."),
        ("memory", "lazy-file-views", 1,
         "Load file-backed DLLs and archives lazily to reduce committed memory."),
        ("memory", "large-pages", 0,
         "Disable 2 MB view alignment to avoid fragmentation on tight address spaces."),
        ("memory", "dxvk-d3d9-maxavailablememory", 3,
         "Cap reported D3D9 texture memory to 512 MiB so the game allocates less."),
        ("memory", "dxvk-dxgi-maxdevicememory", 6,
         "Cap reported DXGI video memory to 1024 MiB."),
        ("threads", "four-cores", 1,
         "Limit workers to 3 cores to reduce concurrent thread pressure."),
        ("threads", "sync", 0,
         "Switch back to standard Wine thread synchronization."),
        ("threads", "dxvk-dxvk-numcompilerthreads", 3,
         "Limit DXVK pipeline compiler threads to 2."),
    ]
    # Carafe's reason for the same change of the same setting, where it has one.
    reasons = {(item["kind"], item["setting"]): item["why"] for item in carafe["suggest"]}
    for kind, sid, val, why in suggestions:
        lines.append(f"suggest\t{kind}\t{sid}\t{val}\t{t15(why, CARAFE.of(reasons.get((kind, CARAFE.ids[sid]))))}")

    # --- Pad outputs & keyboard keys ----------------------------------------
    for idx, label in enumerate(pad_labels, start=1):
        lines.append(f"pad\t0x{idx:x}\t{t15(label, CARAFE.same(label, carafe['pads'][idx - 1]))}")
    # The mouse's thirteen actions are 0x100 and up in both, in the same order.
    mouse = dict(zip(range(0x100, 0x100 + len(carafe["mouse"])), carafe["mouse"]))
    for code, label in key_items:
        lines.append(f"key\t0x{code:x}\t{t15(label, CARAFE.of(mouse.get(code)))}")

    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output", type=Path)
    parser.add_argument("--horizon-wine", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    content = build_menu(args.horizon_wine)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
