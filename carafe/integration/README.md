# Carafe integration

This folder carries everything taken from the [Carafe](https://github.com/carafe-nx/carafe)
project into this Autorun fork, so the two trees can be kept in sync.

## patches-from-carafe/

The 127 runtime patches Carafe applies on top of its pinned Autorun commit
(`636913ca46a3638018b3ea56995d6852db9f0165`, from `autorunhq/autorun`). They come
from `runtime/patches/autorun/` in the Carafe repository (LGPL-2.1, see `LICENSE`).

These are plain unified diffs (not `git format-patch` output); apply them with:

    for p in carafe/integration/patches-from-carafe/*.patch; do git apply --3way "$p"; done

or regenerate a branch with:

    git checkout -b carafe-patches 636913ca
    for p in .../*.patch; do git apply --3way "$p" && git commit -aqm "carafe-patch: $p"; done

## tools/

- `ledger.py` measures, for each patch above, how many of the lines it adds
  are in this tree today: the two numbers of the patch table in PORTING.md.
  `ledger.py --review -v 0023` lists the lines of a patch that are not here
  and says which of them Carafe itself rewrote in a later patch; that needs
  Carafe's patched tree as a git ref
  (`git fetch origin carafe-patches:refs/carafe/patches`).
- `refresh_table.py` writes those numbers into PORTING.md; with `--check` it
  only says whether the table is out of date, which is what the `build`
  workflow runs.

## Status against this fork's branches

| Source | State |
| --- | --- |
| `carafe-nx/autorun` main (`1ac9b6c`) | Already fully contained in `arena/43825775-autorun` — nothing to take. |
| `carafe-nx/carafe` `runtime/patches/autorun` @ `636913ca` | All 127 patches apply cleanly onto `636913ca` (verified as branch `carafe-patches`). Upstream Autorun has since taken many of them in its own form; what the fork has, what it still lacks and the order it is being brought over in are in [PORTING.md](PORTING.md). The folder keeps the full set for diffing and future rebases. |
| `carafe-nx/carafe` app (Tauri/Rust desktop packager), `runtime/loader` | Not taken: they pack a game into an NSP and start it from there, which this fork does without. |
| `carafe-nx/carafe` `runtime/{overlay,orchestrator,fex,dxvk,dxwrapper,media,dshow}` | Being brought over in stages, see [PORTING.md](PORTING.md). |

## Submodule pointer used by Carafe

Carafe pins Autorun at `external/autorun`. To have Carafe pick up this fork,
point that submodule (or Carafe's `runtime/patches/autorun` series) at a branch
here that contains the patch set above.
