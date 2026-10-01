# lw.PPOCR.C geometry extraction (MIT)

Source: https://github.com/lxw112190/lw.PPOCR.C
Reviewed commit: b7d2b42383adcee68b0e4f1a7c3de546be9c9552.
Copyright (c) 2026 天天代码码天天.
Complete license: `licenses/lw-PPOCR-C-MIT.txt` at the project root.

Unmodified snapshots: `ppocr/db_postprocess.c`, `crop.c`, `det_preprocess.c`,
`reading_order.c`, `crop_internal.h`, `reading_order.h`.
Private minimal adapters: `lw_infer.h`, `det_internal.h`, `profile_internal.h`,
`simd/simd_kernels.h`, `simd/scalar_bitmap.c`. They are not a public SDK or the
complete CPU inference implementation. No AVX2 requirement; bitmap interior uses
a consistent four-neighbor rule. Profiling stubs are disabled/no-op, never used
to report timings. Native pipeline timings are measured separately in C++.

Dependencies are hash-pinned; attribution/porting details: root `NOTICE`.
