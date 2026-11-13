Horizon Client @VERSION@
=======================

An in-game menu and mod loader for old TrackMania games, part of TM Horizon.
Open the menu with Insert. MIT licence.

Supported games (any other exe: Horizon stays off and writes why into its log)
  TrackMania Nations ESWC   TmNationsESWC.exe, unofficial 1.80 (0.1.7.9)
  TrackMania Sunrise        TmSunrise.exe of the Win10 community repack
  TrackMania Original       TMORIGINAL.EXE of the community Online Patch v2 (2023),
                            works on top of its dgVoodoo d3d9.dll
  No StarForce versions. TMNF/TMUF: use the TMLoader package.

Install
  1. Unzip everything into the game folder, next to the game's exe.
  2. Double-click Horizon-Install.cmd. It copies horizon\loader\dinput8.dll next
     to the exe. If another mod already has a dinput8.dll there, that file is
     renamed to dinput8.chain.dll and keeps working (Horizon loads it first).
  3. Start the game.

Keys
  Insert        open and close the menu (change it in the menu under General)
  Esc           close the menu
  Ctrl+Insert   move and resize widgets
  Move the mouse to the top screen edge to see the Horizon bar.

If something goes wrong
  Hold Shift while the game starts: safe mode, Horizon runs without modules.
  Hold Ctrl+Shift while the game starts: Horizon is not loaded at all.
  Log: horizon\logs\ (newest file). Settings: horizon\settings\.

Uninstall
  Double-click Horizon-Uninstall.cmd. It removes Horizon's dinput8.dll and gives
  dinput8.chain.dll its old name back. Then delete the horizon folder (your
  settings and logs are in it).

Safety
  - No module changes timing, physics, inputs or replays of a run. No
    checkpoint or trigger display, no ideal or brake line (TMX rules).
  - Autosave writes the replay with the game's own save code, after the run.
  - Maps are only started from the main menu, never online.

Modules (switch them on and off in the menu; not every module runs on every game)
  Autosave & Login-Fix, Controller hot-plug, Input Display, WR Hunter,
  TMX Browser, TMX Upload, Horizon Performance, Horizon Scripts, Node Explorer

Licences of the code Horizon uses: THIRD-PARTY.txt
