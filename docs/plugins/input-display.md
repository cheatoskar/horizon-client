# Input Display

Plugin id `input-display`. ESWC, Sunrise, TMNF and TMUF, online-ok, live toggle, on by default. A HUD window that shows steering, gas and brake as the car sees them, and where the game allows it the speed, gear and RPM.

## What it does

One panel, **Inputs** (HUD: starts popped out and kept open, see `plugin-abi.md`, Panels). While you drive it reads the local player's car once a frame and draws it in one of four styles:

- **Bars**: a steering bar, brake and gas as vertical bars, the speed in the middle, and (with "Graph") a three-second trace of steer, gas and brake.
- **Keys**: four keys (up, left, down, right) that light up when the input passes the "Key at" threshold, with the speed below.
- **Wheel**: a steering wheel that turns with the steering, brake and gas bars at the sides, an RPM arc around it (with "RPM") and the speed below.
- **Minimal**: one line with the speed and three small bars.

Analog values are shown as they are (a pad stick or a pedal shows in between). Outside a race the panel says "Shows while you drive." If the car's values cannot be read it says "No car data in this game yet."

## Settings

On the plugin's page, before the tabs:

| Label | Key | Default | What it does |
|---|---|---|---|
| Style: Bars / Keys / Wheel / Minimal | `Style` (0-3) | Bars | The look |
| Size | `Size` | 1.0x (0.6x to 2.0x) | Scales the panel |
| Key at | `Threshold` | 0.20 (0.05 to 0.90) | Keys style only: how far a stick or pedal goes before its key lights up |
| Speed | `ShowSpeed` | on | Shows the speed |
| mph | `Mph` | off | Speed in mph instead of km/h |
| Graph | `Graph` | on | Bars style only: the three-second trace |
| Gear | `Gear` | on | Shows "gear N" under the speed where the game gives it |
| RPM | `Rpm` | on | Wheel style only: the RPM arc where the game gives it |

Below the settings, "Now" lists the live values (Steer, Gas, Brake, Speed, Gear, RPM; "not available" for a value the game does not give).

## What is read per game

| Game | Steer, gas, brake | Speed | Gear, RPM |
|---|---|---|---|
| ESWC, Sunrise | `CSceneToyVehicle` members `InputSteer`, `InputGas`, `InputBrake` by reflection name (defaults 0xc0, 0xb8, 0xbc if the name is missing) | not read: the engine has only a getter (`LocalCarSpeed`), no field. The panel shows "-". | not read: `CurGear` and `CurRPM` are getters only. Gear and RPM stay off. |
| TMNF, TMUF | the car's `InputSteer`, `InputGas`, `InputBrake` by name if the engine has them; else fixed offsets 0x58 (steer), 0x50 (gas), 0x54 (brake) from TMInterface's `SceneVehicleCar` layout | `CTrackManiaRace.CarSpeedDisplay` (km/h) by name, else offset 0x26c | fixed offsets: gear (int) 0x5c8, RPM (float) 0x5b8, taken from TMInterface's engine block |

Every value is checked before it is shown: steer within -1 to 1, gas and brake within 0 to 1, no NaN; gear 0 to 7; RPM 0 to 20000; speed below 2000. A value that fails the check is treated as not available. The player's car is found through the race object (`HORIZON_ENGINE_RACE`) and the local player's mobil.

## What it never does

No writes to game memory, no change to your input, no hooks, no code patches, no game function calls. Reads are guarded, a bad address gives "not available" and not a crash. This is also the Competition Patch rule on Forever. It does not use the TM Horizon bridge and sends nothing anywhere.

## Where data is stored

Only the settings above, in `horizon\settings\input-display.json`. No data folder use.

## Known limits

- The Forever car offsets (steer, gas, brake, gear, RPM, and the fallback speed offset) come from TMInterface's layout and are only checked for plausibility in my tests against fake game memory. I have not confirmed them in the real game yet. If they are wrong the panel says it cannot read the car (the plausibility check rejects most garbage, but not all of it).
- ESWC and Sunrise have no speed, gear or RPM here, only steer, gas and brake. The values exist in the engine only as getters, and the plugin does not call game code.
- The RPM arc is scaled to a fixed 11000 RPM.
