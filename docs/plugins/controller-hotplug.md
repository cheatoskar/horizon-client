# Controller fix

Plugin id `controller-hotplug`, menu name "Controller fix", version 1.2.1. ESWC, Sunrise and Original, online-ok, live toggle, on by default. A game controller that you plug in (or switch on) after the game has started works at once, with the bindings it would have had if it had been there at start, without a restart.

## What it does

ESWC, Sunrise and Original list the DirectInput game controllers only once, when the game starts (`EnumDevices(DI8DEVCLASS_GAMECTRL, …, DIEDFL_ATTACHEDONLY)`; the game's callback takes every device it is given, it does not filter by device type, and it goes on after a device fails). A pad that comes later is never known to the game; you had to restart. This module registers it while the game runs.

- A background thread waits for Windows device notifications (a HID device arrives or leaves). After each one it lists the game controllers with its own DirectInput object 4 times: 0.5, 2, 5 and 10 s later (a pad's HID device can arrive before DirectInput lists it). If the notifications cannot be registered, it lists every 3 s instead.
- It also asks XInput once per second which pads are connected (`XInputGetCapabilities`: connection state only, no input values). A change there starts the same 4 lists, in case a notification was missed.
- A new controller is registered on the game's main thread through the game's own enumeration callback, the same function the game runs for the pads that are there at start, with the device type DirectInput reported. So the pad appears in the game's input settings, and saved bindings work like for a pad that was there at start.
- A pad that is attached at start but missing from the game's list is registered the same way.
- The game's binding tables get an empty slot for the new device, so nothing reads out of range.
- A toast "Controller added: <name>" shows for a new pad, "Controller back: <name>" for one that is back again. A disconnect is only logged.

### Bindings (1.2.0)

The pad gets its bindings at once, the same way as a pad that is there at start (static analysis of all three exes, addresses in `plugins/controller-hotplug/hp_bind.h` and `hp_bind.cpp`):

- At start the game runs a defaults pass for every profile's bindings config: every keyboard, mouse and pad the config has no binding for gets the game's defaults (`CTrackManiaRace::InputSetDefaultsBindings`, which the app keeps as a callback; for a pad: Accelerate = button 0 and 6, Brake = button 1 and 4, Steer = X axis, Respawn = button 2, Reset = button 5, Horn = button 8). A pad the profile already has bindings for keeps them. Bindings are saved in the profile per device id (a hash of the DirectInput instance GUID), so the same pad on the same USB port is the same device.
- For a late pad the module calls that same defaults pass (ESWC 0x7171d0, Sunrise 0x821d50, Original 0x820230) for the current profile's config. A pad with saved bindings keeps them; a new pad gets the game's defaults, written into the profile like at start.
- In a race, the race's binding table was built at race start without the pad. The module appends the pad's bindings from the config (saved or the game's defaults, the same in both cases) to that table (what the game's own AddBinding writes for the last device), so the pad drives in the running race. The race is the app's `CConstructionGame.CurrentRace` (ESWC +0x140, Sunrise/Original +0x12c, the offsets the host finds by reflection); the module checks that the race uses this input port and that the table belongs to it, and logs the reason if not. Without a race the config is enough: the next race start builds the table with the pad.
- At every race start after a late pad the log says how many bindings it has in the new race table ("race start: <pad> (device 2) has 8 binding(s) in the race table"). A restart of the same race keeps the table.
- 1.2.0 read the race from `CTrackMania.Races` (+0x2f0 / +0x2a8), another object: in game the pad was registered and had its bindings in the profile, but the running race table stayed without them ("race table unchanged").
- The "all objects" tables (pause menu and dialogs) get the pad's menu entries the game's builder adds for every pad: buttons 0-3, the POV and the X/Y axis.
- The log names every binding that was applied with the action and the pad's object id ("bindings for <pad> (id ...): the game's defaults added to the profile, 8 binding(s): Reset=..., Respawn=..., Accelerate=..., ..." and "race table +8, ... menu binding(s) in ... table(s)").

Online-safe: these are the bindings the game itself gives the pad; no input is read, changed or sent. If any of the 14 code signatures per game differs, the bindings part stays off (logged) and the pad is registered as before.

### Xbox controllers

An Xbox controller is a game controller for DirectInput (and so for these games) only while it is switched on: Windows then adds an "XINPUT compatible HID device" for it (VID 045E, `IG_` in its device path). Plugged in by USB but switched off or asleep, only the USB device is there, and neither the game nor this module can see a pad. The module logs that case ("an Xbox controller is plugged in but has no game controller device …") and the page shows "Xbox controller off". Switching it on (Xbox button) is a device arrival like plugging it in: the pad is added.

The first report (2026-11-12) most likely was this case: the Xbox Series controller had been on USB since boot (12:04), but at the start of all four test sessions (Original twice, Sunrise, ESWC, 12:48-13:38) DirectInput listed no controller ("controllers attached: 0"), and Windows dates the last arrival of its XInput HID device to 13:41, after the sessions. Version 1.0.0 logged too little to tell whether the pad was switched on during a session and a notification was missed.

### Unplug and plug in again

The game keeps the pad's slot and tries to get it back on its own. If the game cannot reacquire the device within 2 seconds after it is back, the module gives that slot a fresh DirectInput device for the same instance GUID (same slot, same bindings). This happens at most once per replug.

## Log

Everything the module sees is in the Horizon log, so a report can be read from the log alone:

- at start: the DirectInput list (name, kind such as gamepad or wheel, VID/PID, XInput yes/no, the first 8 digits of the instance GUID), the XInput slots with a pad, the number of Xbox controllers on USB/Bluetooth, and what the game found by itself ("game: 3 input device(s), 1 pad(s)" and each pad);
- each HID arrival/removal with VID/PID (at most 10 lines per 10 s), each XInput connect/disconnect;
- each change of the DirectInput list ("DirectInput: new …", "DirectInput: gone …"), each registration with the game's device count before and after;
- why the game cannot see a pad (Xbox controller switched off; XInput pad that DirectInput does not list).

## Page

"Watching" (device notifications) or "Polling", then one line per controller: name and "In game", "Waiting" or "Unplugged". "No controller" when there is none, "Xbox controller off" or "XInput only" when that is why the game cannot see a pad.

## Settings

None, except the on/off switch in the Horizon menu (or the Horizon Client card in the launcher). Off means the old behaviour for pads plugged in afterwards. Pads that are already registered stay until the game restarts.

## Games and online

| Game | State |
|---|---|
| ESWC, unofficial 1.80 (0.1.7.9) | code path found and checked statically, tests against a fake input port pass; registration of a new pad not seen in game yet |
| Sunrise, Win10 community repack | same code path found statically in `TmSunrise.exe`, not tested in game yet |
| TM Original (Online Patch v2 exe) | same code path found statically, all addresses matched from Sunrise, not tested in game yet |
| TMNF, TMUF | not a target. Tomashu's CoreMod in TMLoader does this there. |

Online-ok: the module never changes input values, never injects input and does not block the game (the lists run on its own thread). It is safe online and for TMX.

## What it never does

No change to input values, no injected input, no remapping, no blocking of the game. It never reads a pad's input state (XInput only for "connected or not"). Its DirectInput lists run on its own thread; a list takes about 150 ms and can delay the game's own `Acquire` calls by up to about 20 ms, which is why it lists only after a device change (or every 3 s when Windows gives no notifications), never on a timer otherwise.

## Known limits

- Bindings are matched like the game does it: by device id. A pad that gets a new instance GUID (another USB port) counts as a new pad and gets the defaults, not the bindings of the same model on the old port.
- Other profiles of the same game get the pad's defaults when the game loads them next time (as at every start).
- Sunrise and Original are untested in game.
- A pad DirectInput does not list at all (an XInput-only device) cannot be added: the games read DirectInput only, and turning XInput into DirectInput input would be input emulation, which the module does not do.

## Standalone build

The module also exists without Horizon: `Controller-Hotplug-<version>-standalone.zip` (a small `dinput8.dll` loader and `controller_hotplug.dll`, log in `controller_hotplug.log` next to the exe). If `controller_hotplug.dll` is loaded in the game, this module stays passive and shows "Controller fix paused: standalone active" as a toast; its page says "Paused". Details: `plugin-abi.md`, "Standalone builds".
