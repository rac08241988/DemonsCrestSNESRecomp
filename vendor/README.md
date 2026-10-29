# Vendored headers

- `librashader/include/` — C headers of [librashader](https://github.com/SnowflakePowered/librashader)
  v0.12.0 (tag `librashader-v0.12.0`, commit 87e8a97), MPL-2.0 (`librashader/LICENSE.md`).
  The game loads `librashader.dll` at runtime through `librashader_ld.h`; it is not linked in.
- `vulkan-headers/include/` — Khronos Vulkan headers (as bundled by SDL 3.4), Apache-2.0
  (SPDX header in each file). Used only for type declarations (`VK_NO_PROTOTYPES`).
- `librashader.dll` (shipped in `shaders/`, not in git) is librashader v0.12.0 (commit 87e8a97, unmodified) built from source with `cargo build --release -p librashader-capi --target x86_64-pc-windows-gnu`; its source is that tag.
