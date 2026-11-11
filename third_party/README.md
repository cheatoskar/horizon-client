# Vendored code

| Folder | What | Version | Licence |
|---|---|---|---|
| `imgui/` | Dear ImGui core + `backends/imgui_impl_dx9`, `imgui_impl_win32` | 1.91.5 (same as TM Vibrant Shaders) | MIT, `imgui/LICENSE.txt` |
| `lucide/` | Lucide icon subset (`assets/lucide-subset.ttf`, built by `tools/make_icon_font.py` from lucide-static 1.62.0) | 1.62.0 | ISC, `lucide/LICENSE` (a few icons derived from Feather: MIT, same file) |
| `stb/` | `stb_image.h` (PNG only) for the logo and icons | 2.30 | public domain or MIT (licence at the end of the file) |

Copied from my legacy-randomizer repo (MIT), commit 7482627, not edited here (update by copying again):

| Folder | What |
|---|---|
| `src/reflect/` | runtime reflection (CMwClassInfo / SMwParamInfo by class and member name) |
| `src/gamectl/` | game control: play a map file, back to the menu, game state, current map |
| `src/core/gbx.*` | GBX header reader used by gamectl |
