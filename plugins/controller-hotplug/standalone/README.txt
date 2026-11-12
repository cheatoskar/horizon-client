Controller Hotplug - detects game controllers plugged in after the game started
==============================================================================

DEUTSCH

Was es ist
  TrackMania Nations ESWC und Sunrise suchen Gamepads nur einmal, beim Start des
  Spiels. Ein Pad, das du spaeter ansteckst, kennt das Spiel nicht; du musstest
  neu starten. Controller Hotplug beobachtet im Hintergrund, ob ein Pad dazukommt,
  und meldet es dem Spiel auf dem Weg, den das Spiel selbst beim Start nimmt. Das
  Pad erscheint dann in den Eingabe-Einstellungen des Spiels. Es veraendert keine
  Eingaben, schickt keine Eingaben und bremst das Spiel nicht (online und fuer
  TMX unbedenklich).

Unterstuetzte Exe (alles andere: es tut nichts und schreibt den Grund ins Log)
  TmNationsESWC.exe, inoffiziell 1.80 (0.1.7.9)
  TmSunrise.exe, Win10-Repack (noch nicht im Spiel getestet)
  TMORIGINAL.EXE, Online Patch v2 (noch nicht im Spiel getestet)

Installieren
  1. Den Inhalt des Zips in den Spielordner entpacken (neben die Exe).
  2. Controller-Hotplug-Install.cmd doppelklicken.
     Gibt es dort schon eine dinput8.dll einer anderen Mod, wird sie zu
     dinput8.chain.dll und laeuft weiter (unser Loader startet sie). Eine fremde
     dinput8.dll wird nie ueberschrieben oder geloescht. Ist die Stelle schon
     belegt (dinput8.chain.dll von einer anderen Mod), aendert das Skript nichts und
     sagt warum.
  3. Spiel starten.

Pruefen, ob es laeuft
  Im Spielordner steht controller_hotplug.log. Dort steht "ready".

Benutzen
  Spiel starten, Pad anstecken, im Spiel unter den Eingabe-Einstellungen einrichten.
  Das Spiel speichert die Belegung pro Geraet. Ein Pad, das schon einmal belegt war,
  bekommt seine Belegung beim naechsten Rennstart (Strecke neu laden). Wird ein Pad
  mitten im Rennen angesteckt, ist es sofort bekannt, die Belegung gilt ab dem
  naechsten Rennstart. Abziehen und wieder anstecken: das Spiel behaelt den Platz
  und die Belegung. Ein Pad an einem anderen USB-Port kann als neues Geraet
  zaehlen und braucht dann eine neue Belegung.
  Xbox-Controller: Das Spiel sieht ihn nur, solange er eingeschaltet ist. Steckt
  er per USB, ist aber aus, steht das im Log; einschalten (Xbox-Taste) reicht,
  er wird dann hinzugefuegt.

Mit Horizon
  Horizon Client hat das als eingebautes Modul "Controller fix".
  Nimm dann das Modul. Sind beide da, pausiert das Modul sich selbst.

Entfernen
  Controller-Hotplug-Uninstall.cmd doppelklicken, oder dinput8.dll und
  controller_hotplug.dll aus dem Spielordner loeschen (eine dinput8.chain.dll
  wieder in dinput8.dll umbenennen). controller_hotplug.log bleibt.

ENGLISH

What it is
  TrackMania Nations ESWC and Sunrise look for game controllers only once, when the
  game starts. A pad you plug in later is unknown to the game; you had to restart.
  Controller Hotplug watches in the background for a new pad and reports it to the
  game the same way the game does at start. The pad then shows up in the game's
  input settings. It never changes input, never sends input and does not block the
  game (safe online and for TMX).

Supported exes (anything else: it does nothing and logs why)
  TmNationsESWC.exe, unofficial 1.80 (0.1.7.9)
  TmSunrise.exe, Win10 repack (not tested in game yet)
  TMORIGINAL.EXE, Online Patch v2 (not tested in game yet)

Install
  1. Unzip into the game folder (next to the exe).
  2. Double-click Controller-Hotplug-Install.cmd.
     If there is a dinput8.dll of another mod already, it becomes
     dinput8.chain.dll and keeps working (our loader runs it). A foreign
     dinput8.dll is never overwritten or deleted. If that spot is taken as well
     (a dinput8.chain.dll of another mod), the script changes nothing and says why.
  3. Start the game.

Check that it runs
  The game folder has controller_hotplug.log. It says "ready".

Use
  Start the game, plug in the pad, set it up in the game's input settings. The game
  saves bindings per device. A pad that was bound before gets its bindings at the
  next race start (load the map again). A pad plugged in mid-race is known at once;
  its bindings apply from the next race start. Unplug and plug in again: the game
  keeps the slot and the bindings. A pad in a different USB port can count as a new
  device and needs new bindings.
  Xbox controllers: the game sees one only while it is switched on. If it is
  plugged in by USB but off, the log says so; switch it on (Xbox button) and it
  is added.

With Horizon
  Horizon Client has this built in as the module "Controller fix".
  Use the module then. If both are there, the module pauses itself.

Remove
  Double-click Controller-Hotplug-Uninstall.cmd, or delete dinput8.dll and
  controller_hotplug.dll from the game folder (rename a dinput8.chain.dll back to
  dinput8.dll). controller_hotplug.log stays.

MIT licence.
