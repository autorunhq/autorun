"""Wine implementations of legacy runtime DLLs needed by the full packages."""

# Include dynamically loaded DLLs that the import walk cannot discover.
LEGACY_RUNTIME_DLLS = (
    'ddraw', 'dinput',
    'd3dx9_25', 'd3dx9_31', 'd3dx9_35', 'd3dx9_36', 'd3dx9_39', 'd3dx9_42',
    'd3dx10_35', 'd3dx10_37', 'd3dx10_39', 'd3dx10_43', 'd3dx11_43',
    'faultrep', 'msvcp80', 'msvcr80', 'msvcp100', 'msvcr100', 'x3daudio1_4',
)
