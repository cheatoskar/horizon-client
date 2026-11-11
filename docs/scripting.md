# Horizon Scripts guide

Horizon Scripts is a Horizon Host plugin (`scripts.dll`) that runs AngelScript 2.38.0 scripts inside the old TrackMania games: TM Nations ESWC, TM Sunrise, TMNF and TMUF. One script folder runs unchanged on all four, because a script only sees the API below and the engine's own member names, never a game address. The language is the one Openplanet and TMInterface use, so the syntax looks familiar (`void Main()`, `[Setting]`, `UI::Text`).

This page is the author guide and the API reference. The plugin contract itself is in `docs/plugin-abi.md`.

## Quick start

1. Open the scripts folder: `<game>\horizon\scripts\`. The Scripts tab has an "Open folder" button.
2. Make a folder in it, for example `hello\`. The folder name is the script id.
3. Put `info.toml` in it:

```toml
name = "Hello"
permissions = ["ui"]
```

4. Put `main.as` next to it:

```angelscript
void Render() {
    UI::SetNextWindowPos(vec2(20, 20), UI::Cond::FirstUseEver);
    if (UI::Begin("Hello", UI::WindowFlags::AlwaysAutoResize)) {
        UI::Text("Hello from " + Game::Title());
        UI::Text("Script time " + Time::FormatDuration(Time::ScriptTime()));
    }
    UI::End();
}
```

5. Save. The folder is polled every 500 ms, so the script loads without a restart. Edit and save again to reload.

Where errors show:

- Horizon menu, page "Horizon Scripts", tab "Scripts": pick the script in the list. "Problems" lists compile errors, warnings and runtime errors with `file:line:column`. "Log" shows what the script printed. "Reload" reloads by hand.
- The host log gets the same lines, prefixed with the script id: `[hello] main.as:3:5: ...`.

Note that `UI::End()` is outside the `if`. `UI::Begin` and `UI::BeginChild` must always be closed, even when they return `false` (same as Dear ImGui).

## Folder layout and manifest

```
<game>\horizon\scripts\<id>\
    info.toml        the manifest (or info.json)
    main.as          any number of .as files, also in sub-folders (3 levels)
<game>\horizon\data\scripts\<id>\
    ...              the script's own data files (IO::), created on the first write
```

- `<id>` is the folder name: letters, digits, space, `_`, `.`, `-`, at most 64 characters, not starting with a dot. Hidden folders and files are skipped.
- Every `.as` file in the folder and its sub-folders (up to 3 levels, at most 512 files looked at, 1 MB per file) is compiled together as one module. The order does not matter, and there is no `#include`: it is rejected with an error.
- `info.toml` (or `info.json`, used only if there is no `info.toml`) must be in the script's folder itself.
- A script is a folder. Loose `.as` files in `horizon\scripts\` are ignored.

### Manifest fields

| Field | Meaning |
|---|---|
| `name` | Required, at most 64 characters. Shown in the list. |
| `author`, `description` | Shown on the script's page. |
| `version` | Text, default `"0.0.0"`. |
| `games` | List of `"eswc"`, `"sunrise"`, `"tmnf"`, `"tmuf"`, `"forever"` (TMNF and TMUF) or `"all"`. Default: all four. `"original"` is ignored with a warning (not supported yet). A script on another game is shown as "Other game" and not loaded. |
| `online` | `"solo-only"` (default) or `"online-ok"`, see Safety and limits. |
| `permissions` | List of `"ui"`, `"settings"`, `"read-game"`, `"game-control"`, see Permissions. Default: none. |

```toml
# Clock: real time and session time in a corner of the screen.
name = "Clock"
author = "cheatoskar"
version = "1.0.0"
description = "The real time and how long the game has been running, in a corner."
games = ["eswc", "sunrise", "tmnf", "tmuf"]
online = "online-ok"
permissions = ["ui", "settings"]
```

The TOML subset that is accepted:

- `key = "text"`, `key = 'text'` (no escapes) or `key = ["a", "b"]`. A list may span lines. A bare word (`key = all`) is read as text.
- `#` starts a comment, also after a value. Keys may be quoted. Names are not case sensitive.
- In `"..."` the escapes are `\n`, `\t`, `\"`, `\\`. Any other escape is an error.
- No `[sections]`, no numbers or booleans as values, and a key set twice is an error.
- An unknown key only gives a warning ("unknown key ... is ignored").

A manifest error shows the script as "Error" with `info.toml` and the line number in Problems.

## Permissions

A script asks for what it needs in `permissions`. The Scripts tab shows them as chips. A script without any permission can still log, read the time and keep its own data files.

| Permission | Unlocks | Without it |
|---|---|---|
| `ui` | Everything in `UI::`, and the `Render()` and `RenderMenu()` callbacks. | `Render()` and `RenderMenu()` are never called (warning at load). A `UI::` call from another place throws `UI::Text needs the permission "ui" in info.toml` and the script is Stopped. |
| `settings` | `[Setting]` globals become a settings page and are saved. | `[Setting]` is ignored (warning at load: add "settings"). The variable stays a normal global. |
| `read-game` | `Game::GetState`, `Game::InRace`, `Game::CurrentMap`, all of `Reflection::` and the reading methods of `Nod`, and the event callbacks (`OnMapLoaded` and the rest). | The event callbacks are never called (warning at load). The functions throw `... needs the permission "read-game" in info.toml`. |
| `game-control` | `Game::PlayMapFile` and `Game::ReturnToMenu`. Offline only, never with Competition Patch. | The two functions throw `... needs the permission "game-control" in info.toml`. |

No permission is needed for: `print`/`warn`/`error`, `Log::`, `Time::`, `IO::`, the `vec` types, the string, array and math add-ons, and `Game::IsOnline`, `Game::Name`, `Game::Title`, `Game::IsForever`, `Game::CanControl`.

`Main()` and `OnUnload()` need no permission either, but they cannot draw (UI calls only work inside `Render` and `RenderMenu`).

## Callbacks

A callback is a global function with exactly this declaration. A function with a callback's name but another signature is never called, and the script gets a warning that shows the expected signature.

| Declaration | When | Permission |
|---|---|---|
| `void Main()` | Once, after the script loaded. Waits while a solo-only script starts paused online. | none |
| `void Render()` | Every frame the host draws overlays, while the script is running. Not while a photo or video is taken (option). | `ui` |
| `void RenderMenu()` | Every frame the script's page ("Script page") is visible in the Scripts tab. | `ui` |
| `void OnMapLoaded(Game::Map@ map)` or `void OnMapLoaded()` | A map was loaded. The first form gets the map, which can be `null`. | `read-game` |
| `void OnRaceStart()` | A run starts, also after a restart. | `read-game` |
| `void OnCheckpoint(int time, int index)` | The player passed a checkpoint (time in ms). | `read-game` |
| `void OnRespawn(int count)` | The player respawned (running count). | `read-game` |
| `void OnFinish(int time, bool isPb)` | The local player finished (time in ms). | `read-game` |
| `void OnRaceEnd()` | The race ended (end screen or replay stop). | `read-game` |
| `void OnMenu()` | The game went back to the menu. | `read-game` |
| `void OnGameState(Game::State state)` | The game state changed. | `read-game` |
| `void OnUnload()` | Before the script is reloaded, switched off or the game quits. Not called after Stopped or Suspended. | none |

`OnMapLoaded(const Game::Map@)` is accepted too. If a script has both forms, only the one with the map is called.

Which game delivers which event, and how, is the table "Which game delivers what" in `docs/plugin-abi.md`, section Events. Short version: `OnRespawn` only on ESWC; `OnCheckpoint` on ESWC and Sunrise; `OnRaceEnd` on ESWC and Sunrise; `OnFinish`, `OnRaceStart`, `OnMapLoaded`, `OnMenu` and `OnGameState` on all four. A callback that a game never fires is simply never called. Do not depend on one of the rare events for the script's main job if it should run everywhere.

Events and `Render()` are only delivered while the script is Running (not Off, Paused, Suspended, Stopped).

### Load order

1. The manifest is read (`info.toml`, then `info.json`).
2. The game gate: wrong game means "Other game", not loaded. Competition Patch on TMNF/TMUF with `game-control`: "Blocked", not loaded. Switched off in the list: "Off".
3. Every `.as` file in the folder is compiled together. Errors stop the load ("Error").
4. Global variable initialisers run, under the time budget (4 times the call limit).
5. `[Setting]` globals are bound and the saved values applied.
6. `Main()` runs once (4 times the call limit). A solo-only script that starts while the game is online starts Paused and runs `Main()` when the game goes offline.

### Hot reload

The folder is polled every 500 ms. When the list of `.as` files, their sizes or their times changed, the script is reloaded once the changed list stayed the same for one more poll, so a half-saved file or an editor writing several files does not reload twice. `OnUnload()` runs first, settings are saved, and the module is rebuilt from scratch: globals start again from their initialisers (saved settings are applied). Deleting the folder unloads the script; a new folder is picked up on the next poll.

## API reference

Declarations are as registered. A heading names the namespace, so `Begin` under `UI::` is called `UI::Begin(...)`. Parameters with `= value` have defaults. Everything runs on the game's main thread.

### Add-ons: string, array, math

These AngelScript add-ons are registered as they come with the SDK:

- `string` (`RegisterStdString`) and its helpers (`RegisterStdStringUtils`: `split`, `join`, ...): [std::string add-on](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_addon_std_string.html). `"a" + 5` works, `s.isEmpty()`, `s.length()`, `s.substr(...)`, `s.findFirst(...)` are there.
- `array<T>` and `T[]` (`RegisterScriptArray`): [array add-on](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_addon_array.html). `foreach (auto x : arr)` works.
- `sin`, `cos`, `sqrt`, `pow`, `floor`, `abs`, ... (`RegisterScriptMath`): [math add-on](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_addon_math.html).

The string add-on also registers a global function `format()`. A script variable, function or parameter named `format` at global scope therefore fails to compile with "Name conflict". Pick another name.

Not registered: dictionary, any, datetime, file, weakref, coroutines (`yield`).

### Global types

```angelscript
vec2(), vec2(float x, float y)                        // float x, y
vec2 opAdd(const vec2 &in) const                      // a + b
vec2 opSub(const vec2 &in) const                      // a - b
vec2 opMul(float) const                               // a * f
float Length() const

vec3(), vec3(float x, float y, float z)               // float x, y, z
vec3 opAdd(const vec3 &in) const
vec3 opSub(const vec3 &in) const
vec3 opMul(float) const
float Length() const

vec4(), vec4(float x, float y, float z, float w)      // float x, y, z, w (colours: r g b a)
vec4 opMul(float) const
```

There is no `==` on vectors, no `float * vec` (write `v * f`), no vector-times-vector and no `vec4` addition. Compare components.

`Nod` is described under Reflection. `Game::Map` and `Game::State` are described under Game.

### Global functions and Log::

```angelscript
void print(const string &in text)           // log line, info
void warn(const string &in text)            // log line, warning
void error(const string &in text)           // log line, error (does not stop the script)
```

```angelscript
namespace Log
void Info(const string &in text)            // same as print
void Warn(const string &in text)            // same as warn
void Error(const string &in text)           // same as error
```

A log line goes to the script's Log in the Scripts tab and to the host log with the script id in front. A script may write at most 20 lines per second into the host log; the rest still shows in the Scripts tab (the last 200 lines are kept), and one line reports how many were not written.

### Time::

```angelscript
namespace Time
uint64 Now()                                // ms since the plugin started
uint64 ScriptTime()                         // ms since this script was loaded
float Delta()                               // ms the last frame took
int RaceTime()                              // ms since the last race start, -1 outside a race
int64 Stamp()                               // Unix time, seconds
string Format(const string &in format)      // local time, see below
string FormatRace(int ms)                   // "0:42.37", "1:02:03.45"; -1 gives "-:--.--"
string FormatDuration(uint64 ms)            // "12:34", "1:02:03"
```

- `RaceTime()` counts on Horizon's own clock from the `RACE_START` event. It is for display, not the game's timer. Take run times from `OnFinish` and `OnCheckpoint`.
- `Format` knows `%H %M %S %I %p %d %m %Y %y %a %A %b %B %j %%`. Any other `%x` is copied as it is.

### UI::

The UI is a small subset of Dear ImGui drawing into the host's ImGui context, so scripts look like the rest of Horizon. It needs the `ui` permission and only works inside `Render()` and `RenderMenu()`.

Rules that every call follows:

- A call outside `Render`/`RenderMenu` throws `UI::Text only works inside Render() and RenderMenu()`.
- Window titles get an invisible suffix per script, so two scripts can both have a window called "Info". A title that contains `##` is used as it is.
- Widgets that edit a value (`Checkbox`, `SliderInt`, `InputText`, ...) take the current value and return the new one. Keep the value in a global.
- Each `Begin`/`Push` call is recorded. A wrong `End`/`Pop` is a script error, not an ImGui assert. See Safety and limits.
- Calls that return `bool` for "is it open" (`BeginTable`, `BeginTooltip`, `TreeNode`) need their `End`/`Pop` only when they returned `true`. `Begin` and `BeginChild` always need theirs.

Enums (values are Dear ImGui's, names are what a script uses, for example `UI::WindowFlags::NoTitleBar`):

| Enum | Values |
|---|---|
| `WindowFlags` | `None`, `NoTitleBar`, `NoResize`, `NoMove`, `NoScrollbar`, `NoCollapse`, `AlwaysAutoResize`, `NoBackground`, `NoSavedSettings`, `NoMouseInputs`, `NoFocusOnAppearing`, `NoNav`, `NoDecoration`, `NoInputs` |
| `Cond` | `Always`, `Once`, `FirstUseEver`, `Appearing` |
| `Col` | `Text`, `TextDisabled`, `WindowBg`, `ChildBg`, `PopupBg`, `Border`, `FrameBg`, `FrameBgHovered`, `FrameBgActive`, `TitleBg`, `TitleBgActive`, `CheckMark`, `SliderGrab`, `Button`, `ButtonHovered`, `ButtonActive`, `Header`, `HeaderHovered`, `HeaderActive`, `Separator`, `PlotHistogram`, `TableHeaderBg`, `TableBorderStrong`, `TableBorderLight`, `TableRowBg`, `TableRowBgAlt` |
| `StyleVar` | `Alpha`, `WindowPadding`, `WindowRounding`, `WindowBorderSize`, `ChildRounding`, `FramePadding`, `FrameRounding`, `ItemSpacing`, `CellPadding` |
| `TableFlags` | `None`, `Resizable`, `RowBg`, `Borders`, `BordersInnerV`, `BordersOuter`, `SizingFixedFit`, `SizingStretchProp`, `NoHostExtendX` |
| `TableColumnFlags` | `None`, `WidthStretch`, `WidthFixed` |
| `Font` | `Body` (0), `Title` (1), `Mono` (2): the host's fonts |

Flags are `int` parameters: combine with `|` (`UI::WindowFlags::NoTitleBar | UI::WindowFlags::NoResize`). `PushStyleColor` and `PushStyleVar` take the enum itself.

#### Windows

```angelscript
bool Begin(const string &in title, int flags = 0)    // always call End(), also when false
void End()
bool BeginChild(const string &in id, const vec2 &in size = vec2(0, 0), bool border = false, int flags = 0)
void EndChild()
void SetNextWindowPos(const vec2 &in pos, int cond = 0)
void SetNextWindowPos(const vec2 &in pos, int cond, const vec2 &in pivot)   // pivot 0..1: (1,1) = bottom right
void SetNextWindowSize(const vec2 &in size, int cond = 0)
void SetNextWindowBgAlpha(float alpha)
vec2 GetWindowPos()
vec2 GetWindowSize()
vec2 GetScreenSize()                                 // the game window, in pixels
vec2 GetContentRegionAvail()
bool IsWindowHovered()
bool IsItemHovered()                                 // the last widget
```

#### Text

```angelscript
void Text(const string &in text)                     // the text is never used as a format string
void TextDisabled(const string &in text)
void TextWrapped(const string &in text)
void TextColored(const vec4 &in color, const string &in text)
void LabelText(const string &in label, const string &in text)
void BulletText(const string &in text)
void SeparatorText(const string &in text)
void SetTooltip(const string &in text)               // for the hovered item
bool BeginTooltip()                                  // EndTooltip() only if true
void EndTooltip()
```

#### Widgets

```angelscript
bool Button(const string &in label, const vec2 &in size = vec2(0, 0))      // true when clicked
bool SmallButton(const string &in label)
bool Checkbox(const string &in label, bool value)                          // returns the new value
bool RadioButton(const string &in label, bool active)                      // true when clicked
bool Selectable(const string &in label, bool selected = false)             // true when clicked
int SliderInt(const string &in label, int value, int min, int max, const string &in format = "%d")
float SliderFloat(const string &in label, float value, float min, float max, const string &in format = "%.3f")
int InputInt(const string &in label, int value, int step = 1)
float InputFloat(const string &in label, float value, float step = 0)
string InputText(const string &in label, const string &in value, int maxLength = 256)
string InputTextMultiline(const string &in label, const string &in value, const vec2 &in size = vec2(0, 0), int maxLength = 4096)
vec3 ColorEdit3(const string &in label, const vec3 &in color)
vec4 ColorEdit4(const string &in label, const vec4 &in color)
int Combo(const string &in label, int current, const array<string> &in items)   // at most 256 items
void ProgressBar(float fraction, const vec2 &in size = vec2(-1, 0), const string &in overlay = "")
bool CollapsingHeader(const string &in label)
bool TreeNode(const string &in label)                // TreePop() only if true
void TreePop()
```

`maxLength` is limited to 1..65536. A slider `format` may hold one number conversion (`%d`, `%i`, `%u` for ints; `%f`, `%e`, `%g` for floats, with flags, width and precision) and `%%`. Anything else (`%s`, two conversions, a longer text than 64 characters) is replaced by the default format without an error.

#### Layout

```angelscript
void SameLine(float offset = 0, float spacing = -1)
void Separator()
void Spacing()
void NewLine()
void Dummy(const vec2 &in size)
void Indent(float width = 0)
void Unindent(float width = 0)
void BeginGroup()
void EndGroup()
void PushItemWidth(float width)
void PopItemWidth()
void SetNextItemWidth(float width)
void PushTextWrapPos(float x = 0)
void PopTextWrapPos()
void AlignTextToFramePadding()
vec2 CalcTextSize(const string &in text)
vec2 GetCursorPos()
void SetCursorPos(const vec2 &in pos)
```

#### Tables

```angelscript
bool BeginTable(const string &in id, int columns, int flags = 0)   // 1 to 64 columns; EndTable() only if true
void EndTable()
void TableNextRow()
bool TableNextColumn()
bool TableSetColumnIndex(int column)
void TableSetupColumn(const string &in label, int flags = 0, float width = 0)
void TableHeadersRow()
```

`TableNextRow`, `TableNextColumn`, `TableSetColumnIndex`, `TableSetupColumn` and `TableHeadersRow` outside a `BeginTable` are a script error.

#### Style

```angelscript
void PushStyleColor(Col color, const vec4 &in value)
void PopStyleColor(int count = 1)
void PushStyleVar(StyleVar var, float value)         // float styles
void PushStyleVar(StyleVar var, const vec2 &in value) // WindowPadding, FramePadding, ItemSpacing, CellPadding
void PopStyleVar(int count = 1)
void PushID(const string &in id)
void PushID(int id)
void PopID()
void BeginDisabled(bool disabled = true)
void EndDisabled()
void PushFont(Font font)
void PopFont()
```

Using the float overload for a vec2 style (or the other way round) is a script error.

#### Drawing

These draw into the background list: under every window, so the Horizon menu stays on top. Coordinates are screen pixels.

```angelscript
void DrawText(const vec2 &in pos, const vec4 &in color, const string &in text, float size = 0)   // size 0 = current font size
void DrawRect(const vec2 &in min, const vec2 &in max, const vec4 &in color, float rounding = 0, float thickness = 1)
void DrawRectFilled(const vec2 &in min, const vec2 &in max, const vec4 &in color, float rounding = 0)
void DrawLine(const vec2 &in a, const vec2 &in b, const vec4 &in color, float thickness = 1)
void DrawCircleFilled(const vec2 &in center, float radius, const vec4 &in color)
```

#### Host

```angelscript
bool IsMenuOpen()                                    // the Horizon menu is open
float GetScale()                                     // the host's UI scale; multiply pixel sizes by it
vec4 AccentColor()                                   // the TM Horizon colours
vec4 TextColor()
vec4 MutedColor()
```

### Game::

```angelscript
namespace Game
enum State { Unknown, MainMenu, SoloRace, EndRace, NetRace, Editor, Replay, Loading }   // 0 to 7

State GetState()                                     // read-game
bool IsOnline()
bool InRace()                                        // read-game: SoloRace or NetRace
Map@ CurrentMap()                                    // read-game: null before the first map and after OnMenu
string Name()                                        // "eswc", "sunrise", "tmnf", "tmuf"
string Title()                                       // the game's display name, as the host reports it
bool IsForever()                                     // TMNF or TMUF
bool CanControl()                                    // game-control granted and allowed right now
bool PlayMapFile(const string &in path)              // game-control
bool ReturnToMenu()                                  // game-control
```

`Game::Map` has these `const` properties:

| Property | Type | Meaning |
|---|---|---|
| `name`, `author`, `uid`, `path`, `environment` | `const string` | `""` when the game does not tell. |
| `authorTime`, `goldTime`, `silverTime`, `bronzeTime` | `const int` | Milliseconds, `-1` unknown. |
| `authorScore` | `const int` | Stunts score, `-1` unknown. |
| `isStunts` | `const bool` | A stunts map (no medal times, use `authorScore`). |

There is one `Map` object. The plugin updates it in place when the next map loads, so copy the values you need in `OnMapLoaded` instead of keeping the handle. Some games leave `author` and `uid` empty for maps started from their own menus, so test `isEmpty()` before showing them.

`PlayMapFile` and `ReturnToMenu`:

- They need `game-control` in the manifest, and they only work when the game is offline, Competition Patch is not loaded, and the host's own run gate allows it. Otherwise they return `false` and write a warning to the log ("... is not allowed now"). `Game::CanControl()` tells in advance.
- They do not run inside the callback. The request is queued and the host runs it at the end of the frame. The `true` result only means "queued". The host log has the real result (`[<id>] PlayMapFile(<path>): <code>`).
- `PlayMapFile` needs a path ending in `.Gbx` (any case) and at most 1024 characters; anything else is a script error.

### Reflection:: and Nod

Reflection reads the engine's own objects by member name, read-only. Class and member names come from the game's reflection tables, so one script works on all games as long as the member exists there. The plugin `node-explorer` lists classes and members and uses the same path syntax.

What it does not do: write, call a getter or a method, or hook anything. It is safe next to Competition Patch and online. It needs `read-game`.

A path is a root and member names joined with dots. Lists take an index in brackets:

```
App.Challenge.ChallengeName
App.CurrentPlayground.GameTerminals[0].ControlledPlayer.Login
App.CurrentPlayground.GameTerminals[0].ControlledPlayer.CurRaceTime
```

- The roots are `App`, `Race` and `Challenge` (case sensitive). A root can be missing right now (no race running); that is a read failure, not an exception.
- Every part except the last must be an object member. The last part is read as a value.
- A member that is a getter or a method (not a plain field) is not read in this version.
- A list member without an index gives its length with `GetCount`, or `"3 items"` with `GetString`.

On failure a typed getter returns `0`, `""`, `false`, an empty `Nod`, or `-1` for `GetCount`, and `Reflection::LastError()` says why ("ChallengeName ... has no member ...", "the object is gone", "the read limit for this frame is used up"). `LastError()` is cleared at the start of each typed read.

```angelscript
namespace Reflection
bool Available()                                     // the engine's class list could be loaded
string LastError()
Nod Root(const string &in name)                      // "App", "Race" or "Challenge"
Nod GetNod(const string &in path)
int GetInt(const string &in path)
uint GetUint(const string &in path)
float GetFloat(const string &in path)
bool GetBool(const string &in path)
string GetString(const string &in path)
vec3 GetVec3(const string &in path)
int GetCount(const string &in path)                  // list length, -1 on failure
```

`Nod` is a value type holding a checked pointer to an engine object. It is checked again on every use, so an object that is gone gives an error instead of a crash. The same getters exist on a `Nod`, with the path relative to it:

```angelscript
Nod();                                               // a null Nod
bool IsNull() const
string ClassName() const                             // "" for null
bool IsKindOf(const string &in className) const      // also true for a parent class
bool opEquals(const Nod &in) const
Nod GetNod(const string &in path) const
int GetInt(const string &in path) const
uint GetUint(const string &in path) const
float GetFloat(const string &in path) const
bool GetBool(const string &in path) const
string GetString(const string &in path) const
vec3 GetVec3(const string &in path) const
int GetCount(const string &in path) const
```

Type conversion: `GetInt`/`GetUint`/`GetBool` accept int, natural, bool, enum, id (and real, truncated). `GetFloat` accepts those and real. `GetString` turns any value into text (an object gives its class name, or `"null"`). `GetVec3` accepts vec3, colour, vec2 (z is 0) and a 3x4 matrix (its position). A value of another type fails with "<path> has another type".

Read limit: every guarded memory read counts. The limit is 4000 per frame by default, shared by all scripts, and can be changed in the plugin's Options tab ("Game reads per frame", 500 to 20000). When it is used up, reads fail until the next frame (and the next event). Read what you need once per frame and keep values in globals.

```angelscript
Nod challenge = Reflection::GetNod("App.Challenge");
string name = challenge.GetString("ChallengeName");
if (name.isEmpty()) print("no name: " + Reflection::LastError());
```

### IO::

Text files in the script's own data folder, `horizon\data\scripts\<id>\`, and nowhere else. No permission is needed. The "Data folder" button in the Scripts tab opens it.

```angelscript
namespace IO
string ReadText(const string &in name)                    // "" if missing or larger than 1 MB
bool WriteText(const string &in name, const string &in text)
bool AppendText(const string &in name, const string &in text)
bool Exists(const string &in name)
bool Delete(const string &in name)
array<string>@ List()                                     // all files, "/" between folders
```

- A name is letters, digits, space, `_`, `.`, `-`, with `/` for sub-folders (at most 4 parts, 128 characters). No drive, no `..`, no backslash, no part starting with a dot or space or ending with a dot or space, no device names (`CON`, `NUL`, `COM1` ...). A bad name is a script error.
- Limits: 1 MB per file, 16 MB and 256 files per script. Going over is a script error.
- `WriteText` and `AppendText` return `false` when the file could not be written.

## Settings

A global variable with a `[Setting ...]` attribute becomes a field on the script's settings page in the Scripts tab, is saved, and is restored on the next start. It needs the `settings` permission. The script reads the variable like any other global.

```angelscript
[Setting name="Show clock"]
bool show = true;

[Setting name="Position" options="Top left|Top right|Bottom left|Bottom right"]
int position = 1;

[Setting name="Background" min=0 max=1 description="Opacity of the box"]
float background = 0.55f;

[Setting name="Note" multiline]
string note = "";
```

| Attribute | Meaning |
|---|---|
| `name="..."` | Label. Default: the variable's name. |
| `description="..."` | Tooltip. |
| `min=` `max=` | With both: a slider, and the value is kept inside the range. With only one of them, or none: a plain input. Numbers are written without quotes. |
| `options="A\|B\|C"` | On an `int`: a combo box. The value is the index (0 for A). Kept inside the list. |
| `hidden` | Saved, but not on the page (for values the script's own page changes). |
| `multiline` | On a `string`: a bigger text box. |

- Supported types: `bool`, `int`, `uint`, `float`, `double`, `string`. Not `const`. Any other type, a `const` variable, or a missing `settings` permission gives a warning and the attribute is ignored. An unknown attribute or a broken number gives a warning; the setting still works.
- The page has a "Reset to defaults" button. The default is the value after the script's own initialiser.
- Values are saved as host settings with the key `s.<id>.<variable>` (in `horizon\settings\scripts.json`), as text. They are written when changed: the page saves at once, and a value that the script changes itself in code is picked up by a check every 2 seconds, and again on unload. A saved value that does not fit the type gives a warning and the default is used.
- Settings belong to the script id, so renaming the folder starts again with the defaults. The on/off switch of a script is stored too (`enabled.<id>`).
- The string field on the page holds at most 1023 characters.
- A script can also draw its own page: `RenderMenu()` is shown under "Script page", between "Settings" and "Log".

## Safety and limits

Scripts run on the game's main thread, inside the game's process, so everything is limited on purpose.

| Limit | Value |
|---|---|
| Time per callback | 25 ms by default. `Main()` and global initialisers get 4 times that. Set in the plugin's Options tab, "Time limit per call", 5 to 200 ms. |
| Over the limit | The call is aborted and the script is "Suspended". |
| Script exception | "Stopped". This includes a missing permission, a bad `UI::` use, a null handle, an array index out of range, and a crash inside a call. |
| Restart | A Suspended or Stopped script stays loaded but is not called again until you save a file or press Reload. |
| Memory | 256 MB for everything AngelScript allocates (all scripts together). Beyond it the script gets an "Out of memory" exception. |
| Stack | 1 MB script stack, 1000 nested calls, 50 nested native calls. |
| Log | 20 lines per second into the host log per script. |
| IO | 1 MB per file, 16 MB and 256 files per script, own folder only. |
| Reflection | 4000 reads per frame by default (Options, 500 to 20000). |
| UI | At most 64 `Begin`/`Push` calls open at once. |

The time check counts script lines. A long call into the engine is not interrupted, so keep native work small.

UI guard: every `Begin` and `Push` a script makes is recorded. A mismatched `End` or `Pop` (for example `UI::EndTable()` while a `UI::Begin` is on top) is a script error with the name of what is still open, instead of a Dear ImGui assert. Whatever a script leaves open at the end of the call, also after an exception or a time abort, is closed by the plugin, so one broken script cannot break the menu or other scripts' windows.

Online gate:

- `solo-only` (the default): while the game is online (LAN or internet race, server browser) the script is Paused. It gets no `Render` and no events, and its `Main()` waits until the game is offline. It resumes by itself.
- `online-ok`: it keeps running online. Use it only for a script that is harmless online, like a clock or an overlay.
- `game-control` calls never work online, whatever `online` says.

Competition Patch (TMNF/TMUF): while `TMCompPatch.dll` is loaded, a script with `game-control` is not loaded ("Blocked"), and one that is already loaded is unloaded when the patch shows up. Scripts that only read (`ui`, `read-game`) keep running. Reading is the same code path as everywhere else: no hooks, no writes.

TMX rules apply to scripts as they do to every plugin (see `docs/plugin-abi.md`, Online gate). A script must not:

- show checkpoints or triggers, or draw an ideal line or a brake line,
- change physics, timing, input or replays of a run,
- disable the camera or MediaTracker behaviour.

The API has nothing for input, timing or physics, and no network access in this version, so these rules are met by what a script can do at all. They are also a rule for what I build with the pieces that exist (overlays, reflection reads).

Overlays are hidden while a photo or video is taken (host flag), unless "Hide script overlays in photos and videos" in the Options tab is off. `RenderMenu()` is not affected.

## Writing for every game

- Use only the API above and engine member names. The game's version differences are in the reflection names; check `Reflection::LastError()` and fall back.
- For the rare line that differs, the compiler defines these words for `#if`:

| Word | Defined when |
|---|---|
| `ESWC` | the game is TM Nations ESWC |
| `SUNRISE` | TM Sunrise |
| `TMNF` | TM Nations Forever |
| `TMUF` | TM United Forever |
| `FOREVER` | TMNF or TMUF |
| `LEGACY` | ESWC or Sunrise |

```angelscript
#if SUNRISE
    print("Sunrise: no Reflection names used here");
#else
    print("name: " + Reflection::GetString("App.Challenge.ChallengeName"));
#endif
```

- Several games leave the map author and uid empty for maps started from their own menus. Check `isEmpty()`, like Map Info does.
- Not every game delivers every event. `docs/plugin-abi.md`, section Events, has the table. If a script needs `OnCheckpoint` it will do nothing on TMNF and TMUF.
- Times are milliseconds, `-1` means unknown. `Time::FormatRace(-1)` gives `-:--.--`.
- The window size depends on the game. Place windows from `UI::GetScreenSize()` and `UI::GetScale()`, not from fixed pixels.

## Differences from Openplanet

- No `yield()` and no coroutines. `Main()` runs once and ends. Work that has to happen every frame goes into `Render()`; work for one event goes into the `On...` callbacks.
- A callback that runs too long is aborted. There is no `sleep`.
- Settings are `[Setting ...]` on globals, like in Openplanet, but the attributes are the ones listed above, and the `settings` permission is required.
- No `Net` (no HTTP, no sockets) in this version.
- No write access to the game. `IO::` only touches the script's own data folder. Reflection is read-only and does not call getters or methods.
- No `#include`: all `.as` files of the folder compile together.
- The `UI::` namespace is smaller than Openplanet's and works on Dear ImGui's own widgets. There is no plugin-wide menu entry; the script's own page is `RenderMenu()`.
- Permissions are declared in `info.toml`, and a script runs only on the games it lists.

## Examples

Both live in `plugins\scripts\examples\`. Copy a folder into `<game>\horizon\scripts\` to try it.

### clock

Real time and session time in a corner. Permissions `ui` and `settings`; no game access, so it runs everywhere, also online (`online = "online-ok"`).

- Settings: show, corner (`options` combo), 24 or 12 hours, seconds, session time, large text, background (`min=0 max=1` slider), margin.
- `ClockText()` builds the format text and calls `Time::Format`:

```angelscript
string ClockText() {
    string f = hours == 0 ? "%H:%M" : "%I:%M";
    if (seconds) f += ":%S";
    if (hours == 1) f += " %p";
    return Time::Format(f);
}
```

- `PlaceWindow()` turns the corner choice into a position and a pivot, using `UI::GetScreenSize()` and `UI::GetScale()`.
- `Render()` opens a borderless, auto-sized, click-through window and draws the time, then the session line with `Time::FormatDuration(Time::Now())` in `UI::MutedColor()`:

```angelscript
int flags = UI::WindowFlags::NoDecoration | UI::WindowFlags::AlwaysAutoResize | UI::WindowFlags::NoInputs
          | UI::WindowFlags::NoSavedSettings | UI::WindowFlags::NoFocusOnAppearing | UI::WindowFlags::NoNav;
if (UI::Begin("Clock", flags)) { ... }
UI::End();
```

### map-info

Name, author and medal times of the map that is running. Permissions `ui`, `settings`, `read-game`.

- `OnMapLoaded(Game::Map@ map)` copies the values into globals (the map object is reused), `OnMenu()` clears them.
- `Main()` picks up a map that is already running when the script is loaded or reloaded:

```angelscript
void Main() {
    Game::Map@ map = Game::CurrentMap();
    if (map !is null) OnMapLoaded(map);
}
```

- The author line is left out when `mapAuthor.isEmpty()`, because some games do not tell it for maps from their own menus.
- Medal times are a two-column table (`UI::BeginTable("medals", 2, UI::TableFlags::SizingFixedFit)`), one `MedalRow` per medal; rows with `-1` are skipped. On a stunts map it shows `authorScore` instead.
- "Hide while driving" uses `Game::InRace()`.

## Testing without the game

After `build.cmd`, run from the repo root:

```
plugins\scripts\tests\run_tests.cmd
```

It runs two programs:

- `build\test\scripts\scripts_test.exe`: unit and engine tests (manifest, gate, time budget, settings metadata, file names, the engine with test scripts, Competition Patch rule).
- `build\test\scripts\scripts_fakehost.exe build\plugins\scripts\scripts.dll plugins\scripts\examples`: loads `scripts.dll` in a stub host once per game (ESWC, Sunrise, TMNF, TMUF), copies the examples folder into that game's `horizon\scripts\`, fires a race's events (map, start, checkpoint, finish, end, menu), draws frames with the overlay, the Scripts panel and the Options panel, and checks that both examples ran unchanged, drew their windows and logged nothing bad.

The exit code is non-zero when a check fails. The fake host has no real engine objects, so `Reflection::` finds nothing there: a script that reads the game has to handle failure (and it should anyway). Real reads are only testable in the game.
