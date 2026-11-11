# Testplan Horizon Host 0.1.0 (11.11.2026)

Nur die Testkopien, nie die echten Ordner. Bei jedem Test entsteht `horizon\logs\<Datum_Uhrzeit>.log` im Spielordner. Wenn etwas nicht geht: genau diese Datei schicken.

**Tasten:** Menü **Einfg (Insert)**. Die Num-0-Taste (Einfg bei ausgeschaltetem NumLock) zählt absichtlich nicht, weil TM das Ziffernfeld für Kameras nutzt. Die Taste lässt sich im Menü unter Horizon › General ändern: auf Change klicken und die neue Taste drücken, oder eine der Vorschläge wählen (F2, #, +, Strg+Tab). Esc schließt das Menü, ohne dass das Spiel das Esc bekommt. Maus an den oberen Bildschirmrand: die schmale Horizon-Leiste.

**Start ohne Mods:** beim Spielstart Shift halten (Host läuft, keine Plugins). Strg+Shift: Horizon wird gar nicht geladen.

## 1. ESWC so wie installiert (mit altem Autosave in der Kette)

Ordner `C:\Users\osaro\Games\TMN-ESWC-horizon-test`, Start `TmNationsESWC.exe`.
Was dort liegt: `dinput8.dll` = Horizon Legacy Loader, `dinput8.chain.dll` = dein alter Autosave-Loader 1.2 (vorher dinput8.dll), `d3d9.dll` = TMVS wie in deinem Ordner, `horizon\` = Host + Autosave-Plugin.

1. Spiel starten, Login wie immer. Im Hauptmenü Einfg drücken: großes Menü mit Seitenleiste (MODS, HORIZON, OTHER MODS).
2. ESWC Autosave steht auf **Off** mit dem Hinweis, dass der alte Loader aktiv ist (sonst würde doppelt gespeichert). Unter OTHER MODS: „ESWC Autosave (old loader)“ und „TM Vibrant Shaders“.
3. Menü zu, F8: TMVS-Menü geht wie gewohnt. Beide Menüs zusammen offen: sag mir, ob sich etwas überlagert oder flackert.
4. Ein Solo-Rennen fahren: Das alte Autosave speichert wie immer (eswc_autosave.log).
5. Maus an den oberen Rand: Leiste fährt ein, Klick auf „Menu“ öffnet das Menü.

Im Log suchen:
- `chain: dinput8.chain.dll ... it is the ESWC Autosave loader`
- `d3d: device ... hooked: Present and Reset`, `overlay: ready ... menu key Insert`
- `hook: CTrackManiaRace::OnFinishLine ... chained onto another mod's jump` (Horizon hängt sich hinter den alten Autosave)
- `event: map loaded`, `event: race start`, `event: checkpoint N at ... ms`, `event: finish ... ms`, `event: race end (solo end screen ...)`
- `online: fix NOT active` ist hier normal: das macht der alte Autosave schon.

## 2. ESWC mit dem Autosave-Plugin statt des alten Loaders

1. Im Menü ESWC Autosave auf **On** schalten (Hinweis „applies after a restart“ ist richtig), Spiel beenden.
2. Im Ordner `dinput8.chain.dll` umbenennen in `dinput8.chain.dll.off`.
3. Spiel starten. ESWC Autosave steht auf On; seine Seite zeigt Mode (best/all), die Häkchen und „Save now“ (F9).
4. Solo fahren bis ins Ziel (neue Bestzeit): Replay liegt in „My Replays“; auf der Seite steht „Saved: …“.
5. Online/LAN: nach dem Rennen speichert es das vorige Replay (wie früher), plus TMX-Kopie bei mehreren Geistern.

Im Log: `[eswc-autosave] started`, `hook: ... OnFinishLine ... installed`, `[eswc-autosave] [Solo] SAVED (finish): ...`, `online: fix active` (Login muss weiter klappen).

## 3. Sunrise

Ordner `C:\Users\osaro\Games\TMSunrise-horizon-test`, Start `TmSunrise.exe` (hat seinen eigenen d3d9.dll-Wrapper, Horizon arbeitet damit).
Kurz von mir automatisch getestet (ohne Online-Login, Prozess danach beendet): Gerät gehookt, Overlay bereit, Reset auf 2560x1440 ok, Spielzustand „Main menu“ erkannt. Bitte prüfen:
1. Einfg: Menü erscheint (keine Plugins installiert, also leere MODS-Liste mit Hinweis), Leiste am oberen Rand.
2. Ein Rennen starten: Log zeigt `event: map loaded "<Name>"` und `event: game state Solo race`. Ziel/Checkpoints gibt es auf Sunrise noch nicht (steht so im Log).

## 4. Forever über TMLoader (experimentell, nur wenn Zeit ist)

Nicht installiert (dein TMLoader-Ordner bleibt unberührt). Zum Testen: `C:\Users\osaro\Downloads\horizon-host\dist\tmloader\Horizon Host` nach `%LOCALAPPDATA%\TMLoader\database\TmForever\products\` kopieren, im TMLoader „Horizon Host“ anhaken, starten. Erwartet: Menü und Leiste, keine Spiel-Hooks, Eingabe wird nie blockiert (Competition Patch). Log: `...\products\Horizon Host\logs\`. Danach wieder abhaken. Mit TMVS + CenterSpeed zusammen: sag mir, ob beide Overlays laufen.

## Entfernen

`Horizon-Uninstall.cmd` im Spielordner (macht aus `dinput8.chain.dll` wieder `dinput8.dll`), danach den Ordner `horizon` löschen. Oder einfach die Testkopie löschen.
