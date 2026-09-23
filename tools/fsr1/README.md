# AMD FidelityFX Super Resolution 1

`ffx_a.h` and `ffx_fsr1.h` are unmodified from
https://github.com/GPUOpen-Effects/FidelityFX-FSR (v1.20210629, MIT, see
`LICENSE.txt`). `tools/fshack_easu.comp` and `tools/fshack_rcas.comp` include
them for the Upscaling setting's FSR 1.0 mode, and
`tools/make_fshack_shaders.py` compiles both into
`dlls/win32u/fshack_fsr_spv.h`.
