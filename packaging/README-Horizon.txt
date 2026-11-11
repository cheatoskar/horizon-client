Horizon Client - in-game mod host of TM Horizon (test build)
==============================================================

DEUTSCH

Installieren (ESWC, Sunrise, Original, Forever ohne TMLoader)
  1. Den Inhalt des Zips in den Spielordner entpacken (neben TmNationsESWC.exe,
     TmSunrise.exe, TMORIGINAL.EXE bzw. TmForever.exe).
  2. Horizon-Install.cmd doppelklicken. Eine vorhandene dinput8.dll einer anderen
     Mod wird zu dinput8.chain.dll und laeuft weiter.
  3. Spiel starten. Menue: Einfg (Insert). Maus an den oberen Bildschirmrand: die
     Horizon-Leiste.
  Ohne Mods starten: beim Start Shift halten (Strg+Shift: Horizon gar nicht laden).
  Log: horizon\logs\<Datum>.log, Einstellungen: horizon\settings\.
  Entfernen: Horizon-Uninstall.cmd, danach den Ordner horizon loeschen.

Forever mit TMLoader
  Den Ordner "TMLoader\Horizon Host" nach
  %LOCALAPPDATA%\TMLoader\database\TmForever\products kopieren und "Horizon Client"
  im TMLoader anhaken. Experimentell: auf Forever nur Menue, Leiste und Plugins,
  keine Spiel-Hooks, die Eingabe wird nicht angefasst (Competition Patch).

ENGLISH

Install (ESWC, Sunrise, Original, Forever without TMLoader)
  1. Unzip into the game folder (next to TmNationsESWC.exe, TmSunrise.exe,
     TMORIGINAL.EXE or TmForever.exe).
  2. Double-click Horizon-Install.cmd. An existing dinput8.dll of another mod
     becomes dinput8.chain.dll and keeps working.
  3. Start the game. Menu: Insert. Move the mouse to the top screen edge for the
     Horizon bar.
  Start without mods: hold Shift while the game starts (Ctrl+Shift: no Horizon at all).
  Log: horizon\logs\<date>.log, settings: horizon\settings\.
  Remove: Horizon-Uninstall.cmd, then delete the horizon folder.

Forever with TMLoader
  Copy the folder "TMLoader\Horizon Host" into
  %LOCALAPPDATA%\TMLoader\database\TmForever\products and tick "Horizon Client" in
  TMLoader. Experimental: on Forever only the menu, the bar and plugins; no game
  hooks, input is never touched (Competition Patch).

Supported exes (anything else: the host stays off and logs why)
  TrackMania Nations ESWC, unofficial 1.80 (TmNationsESWC.exe 0.1.7.9)
  TrackMania Sunrise, Win10 community repack
  TrackMania Original, TMORIGINAL.EXE of the community Online Patch v2 (2023)
  (works on top of its dgVoodoo d3d9.dll; any other TM Original exe is refused)
  TrackMania Nations/United Forever 2.11.26, TMLoader 2.12.0 / 2.12.0-compat
  No StarForce versions.

MIT licence. Dear ImGui (MIT, Omar Cornut).
