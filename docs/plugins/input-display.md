# Input Display

Plugin id `input-display`. ESWC, Sunrise, Original, TMNF and TMUF, online-ok, live toggle, off by default (switch it on in the menu; it loads right away). A HUD window that shows steering, gas and brake as the car sees them, and where the game allows it the speed, gear and RPM.

## What it does

One panel, **Inputs** (HUD: starts popped out and kept open, see `plugin-abi.md`, Panels). The widget has no window background (`HORIZON_PANEL_NO_BACKGROUND`): with the menu closed only the widget itself is on screen, so it can sit bare in a corner; with the menu open or in layout mode its frame shows so it can be moved. The window is exactly as large as the style draws. The page header has a switch to show or hide it. While you drive it reads the local player's car once a frame and draws it in one of five styles:

- **Arrows** (default): four keys (up, left, down, right) that light up when the input passes the "Key at" threshold, with the speed below.
- **Controller**: the classic input display. Two steering triangles left and right, filled from the middle by the steering; gas above and brake below between them.
- **Bars**: a steering bar, brake and gas as vertical bars, the speed in the middle, and (with "Graph") a three-second trace of steer, gas and brake.
- **Wheel**: a steering wheel that turns with the steering, an RPM arc around it (with "RPM") and the speed below. Brake and gas bars at the sides only for analog pedals.
- **Minimal**: one line with the speed and three small bars.

Analog values are shown as they are (a pad stick or a pedal shows in between). An input counts as analog once it held a value in between for six frames in a row; until then it is on/off (keyboard, pad buttons) and is drawn steady: it stays on for 100 ms after it went off, so a value that drops to 0 for a frame does not make it flash. Outside a race nothing is drawn while the menu is closed; with the menu open the widget shows its shape dimmed with "Waiting for race". If the car's values cannot be read it says "No data".

## Settings

On the plugin's page, before the tabs:

| Label | Key | Default | What it does |
|---|---|---|---|
| Style: Arrows / Controller / Bars / Wheel / Minimal | `Style` (0 Bars, 1 Arrows, 2 Wheel, 3 Minimal, 4 Controller) | Arrows (0.1 settings: Bars) | The look |
| Size | `Size` | 1.0x (0.6x to 2.0x) | Scales the panel |
| Key at | `Threshold` | 0.20 (0.05 to 0.90) | Arrows style only: stick or pedal travel that lights a key |
| Speed | `ShowSpeed` | on | Shows the speed |
| mph | `Mph` | off | Speed in mph instead of km/h |
| Graph | `Graph` | off (0.1 settings: on) | Bars style only: the three-second trace |
| Gear | `Gear` | on | Shows "gear N" under the speed where the game gives it |
| RPM | `Rpm` | on | Wheel style only: the RPM arc where the game gives it |

Below the settings, "Now" lists the live values (Steer, Gas, Brake, Speed, Gear, RPM; "-" for a value the game does not give) and whether the pedals count as analog.

Settings saved by 0.1 (any of the keys above set): the old defaults Bars and Graph are written once for the keys never changed, so the look stays (`DefaultsV` = 2 marks that this ran).

## What is read per game

| Game | Steer, gas, brake | Speed | Gear, RPM |
|---|---|---|---|
| ESWC, Sunrise, Original | `CSceneToyVehicle` members `InputSteer`, `InputGas`, `InputBrake` by reflection name (defaults 0xc0, 0xb8, 0xbc if the name is missing) | not read: the engine has only a getter (`LocalCarSpeed`), no field. The panel shows "-". | not read: `CurGear` and `CurRPM` are getters only. Gear and RPM stay off. |
| TMNF, TMUF | the car's `InputSteer`, `InputGas`, `InputBrake` by name if the engine has them; else fixed offsets 0x58 (steer), 0x50 (gas), 0x54 (brake) from TMInterface's `SceneVehicleCar` layout | `CTrackManiaRace.CarSpeedDisplay` (km/h) by name, else offset 0x26c | fixed offsets: gear (int) 0x5c8, RPM (float) 0x5b8, taken from TMInterface's engine block |

Every value is checked before it is shown: steer within -1 to 1, gas and brake within 0 to 1, no NaN; gear 0 to 7; RPM 0 to 20000; speed below 2000. A value that fails the check is treated as not available. The player's car is found through the race object (`HORIZON_ENGINE_RACE`) and the local player's mobil.

## What it never does

No writes to game memory, no change to your input, no hooks, no code patches, no game function calls. Reads are guarded, a bad address gives "-" and not a crash. This is also the Competition Patch rule on Forever. It does not use the TM Horizon bridge and sends nothing anywhere.

## Where data is stored

Only the settings above, in `horizon\settings\input-display.json`. No data folder use.

## Known limits

- The Forever car offsets (steer, gas, brake, gear, RPM, and the fallback speed offset) come from TMInterface's layout and are only checked for plausibility in my tests against fake game memory. I have not confirmed them in the real game yet. If they are wrong the panel says it cannot read the car (the plausibility check rejects most garbage, but not all of it).
- ESWC, Sunrise and Original have no speed, gear or RPM here, only steer, gas and brake. The values exist in the engine only as getters, and the plugin does not call game code.
- The RPM arc is scaled to a fixed 11000 RPM.
