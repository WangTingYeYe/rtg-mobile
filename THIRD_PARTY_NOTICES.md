# Third-party notices

This is a modified, Android-focused adaptation of the MIT-licensed d2r-mobile compatibility layer. Upstream authorship and component licenses are retained. No original game program, resource package or font is distributed with this source tree.

| Component | Source / origin | License and notice |
| --- | --- | --- |
| Compatibility layer (`rtg/`, parts of `tools/`) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), Copyright (c) 2026 jhaizhou; adapted for RTG | [MIT](LICENSE) |
| BC texture decoder | [bcdec](https://github.com/iOrange/bcdec), 0.985, upstream commit `80859ed` | [MIT or Unlicense](rtg/third_party/bcdec/LICENSE) |
| Shader translation libraries | Ryujinx 1.3.2 subset: Ryujinx.Graphics.Shader, Ryujinx.Common, Spv.Generator | [MIT](rtg/third_party/ryujinx/LICENSE.txt), [Spv.Generator notice](rtg/third_party/ryujinx/src/Spv.Generator/LICENSE) |
| NVN interface declarations | h1k421, Copyright (c) 2020 | MIT notice in [nvn.h](rtg/gfx/nvn/include/nvn.h) |
| Texture layout algorithm | [deko3d](https://github.com/devkitPro/deko3d), Copyright (C) 2018–2020 fincs; ported and adapted in `rtg/gfx/nvn/nvn_tex.c` | [zlib notice](LICENSES/deko3d.txt) |

The C# translator wrapper and RTG compatibility changes are modified code, not unmodified upstream releases. The vendored Ryujinx library subset retains its upstream notices. Build dependencies installed separately (Android SDK/NDK, .NET, SPIRV-Tools, SPIRV-Cross, Python packages) retain their own licenses.
