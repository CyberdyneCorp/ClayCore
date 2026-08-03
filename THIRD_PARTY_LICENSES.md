# Third-party dependencies

Policy (openspec `build-packaging`): permissive licenses only — MIT, BSD,
zlib, Apache-2.0, BSL-1.0. No Boost, no GPL/LGPL, no exceptions across the
ABI. Every `FetchContent_Declare` in `cmake/Dependencies.cmake` must have a
row here; `tools/check_licenses.py` enforces both directions in CI.

| Dependency | Pinned version | License | Homepage |
|---|---|---|---|
| doctest | v2.5.3 | MIT | https://github.com/doctest/doctest |
| meshoptimizer | v1.2 | MIT | https://github.com/zeux/meshoptimizer |
| xsimd | 14.3.0 | BSD-3-Clause | https://github.com/xtensor-stack/xsimd |
| benchmark | v1.9.5 | Apache-2.0 | https://github.com/google/benchmark |
| ufbx | v0.23.0 | MIT | https://github.com/ufbx/ufbx |
| metalcpp | macOS15.2_iOS18.2 | Apache-2.0 | https://developer.apple.com/metal/cpp/ |

Planned (added when their consuming module lands, per the same policy):
nanobind (BSD-3-Clause, pyclay), cgltf or tinyply (MIT, glTF/PLY). assimp
is used in CI only as an independent export validator and is never linked
into the shipping library.
