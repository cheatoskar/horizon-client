# Vendored code

| Folder | What | Version | Licence |
|---|---|---|---|
| `imgui/` | Dear ImGui core + `backends/imgui_impl_dx9`, `imgui_impl_win32` | 1.91.5 (same as TM Vibrant Shaders) | MIT, `imgui/LICENSE.txt` |
| `lucide/` | Lucide icon subset (`assets/lucide-subset.ttf`, built by `tools/make_icon_font.py` from lucide-static 1.62.0) | 1.62.0 | ISC, `lucide/LICENSE` (a few icons derived from Feather: MIT, same file) |
| `stb/` | `stb_image.h` (PNG only) for the logo and icons | 2.30 | public domain or MIT (licence at the end of the file) |
| `angelscript/` | AngelScript library (`include/`, `source/`) and the add-ons `scriptarray`, `scriptbuilder`, `scriptstdstring` (+ `_utils`), `scriptmath` (without `scriptmathcomplex`), for the Horizon Scripts plugin (`plugins/scripts`). Not edited. | 2.38.0 | zlib, `angelscript/LICENSE.txt` |

AngelScript source: the official SDK zip `angelscript_2.38.0.zip` from https://www.angelcode.com/angelscript/downloads.html
(SHA-256 `b33b5dbcda10317ef67d628353d83246984ce6fcac102d4dc2aed121eba52e6f`, zip dated 2025-08-08; the same release is
tag `2.38.0` in the official SVN, https://svn.code.sf.net/p/angelscript/code/tags/2.38.0/). Copied from `sdk/angelscript/`
and `sdk/add_on/`; the other add-ons, samples, docs and project files are left out. Built as a static library by
`plugins/scripts/build_plugin.cmd` (`/W3`, its warnings are not ours; our code includes its headers as external).

Copied from my legacy-randomizer repo (MIT), commit 7482627, not edited here (update by copying again):

| Folder | What |
|---|---|
| `src/reflect/` | runtime reflection (CMwClassInfo / SMwParamInfo by class and member name) |
| `src/gamectl/` | game control: play a map file, back to the menu, game state, current map |
| `src/core/gbx.*` | GBX header reader used by gamectl |
