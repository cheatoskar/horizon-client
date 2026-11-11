# Controller nachträglich erkennen

Plugin id `controller-hotplug`, menu name "Controller nachträglich erkennen", version 1.0.0. ESWC and Sunrise, online-ok, live toggle, on by default. A game controller that you plug in after the game has started shows up in the game's input settings, without a restart.

## What it does

ESWC and Sunrise list the DirectInput game controllers only once, when the game starts. A pad plugged in later is never known to the game; you had to restart. This module registers it while the game runs.

- A background thread waits for Windows device notifications (a HID device arrives or leaves). If the notifications cannot be registered, it falls back to one DirectInput enumeration per second.
- On a change it lists the game controllers with its own DirectInput object.
- A new controller is registered on the game's main thread through the game's own enumeration callback, the same function the game runs for the pads that are there at start. So the pad appears in the game's input settings, and saved bindings work like for a pad that was there at start.
- The game's binding tables get an empty slot for the new device, so nothing reads out of range.
- A toast "Controller erkannt: <name>" shows for a new pad and for one that is back again. The host log gets a line each time. A disconnect is only logged.

Bindings are saved by the game per device (a hash of the DirectInput instance GUID). A pad that was bound before gets its bindings at the next race start (when the map loads). A pad plugged in mid-race is known at once; its bindings apply from the next race start (load the map again).

### Unplug and plug in again

The game keeps the pad's slot and tries to get it back on its own. If the game cannot reacquire the device within 2 seconds after it is back, the module gives that slot a fresh DirectInput device for the same instance GUID (same slot, same bindings). This happens at most once per replug.

## Settings

None, except the on/off switch in the Horizon menu (or the Horizon Client card in the launcher). Off means the old behaviour for pads plugged in afterwards. Pads that are already registered stay until the game restarts.

## Games and online

| Game | State |
|---|---|
| ESWC, unofficial 1.80 (0.1.7.9) | code path found and checked statically, tests against a fake input port pass; not tested in game yet |
| Sunrise, Win10 community repack | same code path found statically in `TmSunrise.exe`, not tested in game yet |
| TrackMania Original | not supported (the host does not support Original yet) |
| TMNF, TMUF | not a target. Tomashu's CoreMod in TMLoader does this there. |

Online-ok: the module never changes input values, never injects input and does not block the game (the enumeration runs on its own thread). It is safe online and for TMX.

## What it never does

No change to input values, no injected input, no blocking of the game (the enumeration runs on its own thread).

## Known limits

- Bindings of a pad that is new to the game apply from the next race start, not in the race where it was plugged in.
- Sunrise is untested in game.
- TrackMania Original is not supported.
- A pad that gets a new instance GUID after being plugged into another USB port counts as a new device and needs new bindings.

## Standalone build

The module also exists without Horizon: `Controller-Hotplug-<version>-standalone.zip` (a small `dinput8.dll` loader and `controller_hotplug.dll`, log in `controller_hotplug.log` next to the exe). If `controller_hotplug.dll` is loaded in the game, this module stays passive and shows "Eigenständiges Controller-Hotplug gefunden – Modul pausiert" as a toast and on its page. Details: `plugin-abi.md`, "Standalone builds".
