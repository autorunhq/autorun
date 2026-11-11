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

## Status against this fork's branches

| Source | State |
| --- | --- |
| `carafe-nx/autorun` main (`1ac9b6c`) | Already fully contained in `arena/43825775-autorun` — nothing to take. |
| `carafe-nx/carafe` `runtime/patches/autorun` @ `636913ca` | All 127 patches apply cleanly onto `636913ca` (verified as branch `carafe-patches`). Most of their content is already carried by this fork's own Carafe work (`b8eb2cb..ed58909`); the folder keeps the full set for diffing and future rebases. |
| `carafe-nx/carafe` app (Tauri/Rust desktop packager), `runtime/{fex,dxvk,loader,...}`, docs | Not applicable inside the Autorun tree — they live in Carafe's own build system and consume Autorun as a submodule. The link back is described below. |

## Submodule pointer used by Carafe

Carafe pins Autorun at `external/autorun`. To have Carafe pick up this fork,
point that submodule (or Carafe's `runtime/patches/autorun` series) at a branch
here that contains the patch set above.
