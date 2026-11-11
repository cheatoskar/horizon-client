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

## 2b. ESWC Entwicklerstand (Horizon 0.2.0 + TMVS mit Handshake + Node Explorer)

Ordner: `C:\Users\osaro\Games\TMN-ESWC-horizon-next` (Kopie von TMN-ESWC-horizon-test). Darin: Horizon Host 0.2.0 vom Branch `next` (gleicher Build wie in TMSunrise-horizon-next), das alte Autosave weiter als `dinput8.chain.dll`, TM Vibrant Shaders vom Branch horizon-coop (8e2a099 + a3dcd02) als `d3d9.dll` (das bisherige liegt als `d3d9.dll.tmvs-1.5-test` daneben), Node Explorer 0.1.0, TMX Upload (aus). Die getesteten Ordner bleiben auf 0.1.0. ESWC meldet sich beim Start online an wie sonst auch.

1. Spiel starten. Horizon-Log (`horizon\logs\`, neueste Datei): `[node-explorer] started: Node Explorer 0.1.0 ...`, unter Other mods steht TM Vibrant Shaders. TMVS-Log (`tmvs.log`, Ordner über das TMVS-Menü): `horizon: Horizon Host 0.2.0 found (host.dll): our menu gives way to its menu, ...`.
2. TMVS-Menü (F8) öffnen, dann Insert: das TMVS-Menü verschwindet, solange das Horizon-Menü offen ist (tmvs.log: `menu: hidden while the Horizon menu is open`), und kommt nach dem Schließen zurück. F7 (Effekte an/aus) geht weiter, auch bei offenem Horizon-Menü.
3. Ein Rennen fahren und mit F3 einen Clip speichern, während die Horizon-Leiste oben zu sehen ist (Maus an den oberen Rand): im Clip ist keine Horizon-Leiste und kein Horizon-Fenster. F12-Foto: ebenso.
4. Insert → Node Explorer → App: die Werte des Spiels erscheinen (z. B. die laufende Strecke), ändern sich beim Kartenwechsel, nichts lässt sich schreiben.
5. Aussehen: Knöpfe sind heller als die Karten, General/Hotkeys/Log/About haben Symbole, Node Explorer und TMX Upload stehen unter Mods (TMX Upload aus).
6. Altes Autosave: speichert nach dem Ziel wie bisher (`eswc_autosave.log`). Das Autosave-Plugin bleibt aus, solange `dinput8.chain.dll` da ist.
7. Bei Problemen: neueste Horizon-Log-Datei und `tmvs.log` schicken. Zurück: wieder TMN-ESWC-horizon-test benutzen.

## 3. Sunrise

Ordner `C:\Users\osaro\Games\TMSunrise-horizon-test`, Start `TmSunrise.exe` (hat seinen eigenen d3d9.dll-Wrapper, Horizon arbeitet damit).
Kurz von mir automatisch getestet (ohne Online-Login, Prozess danach beendet): Gerät gehookt, Overlay bereit, Reset auf 2560x1440 ok, Spielzustand „Main menu“ erkannt. Bitte prüfen:
1. Einfg: Menü erscheint (keine Plugins installiert, also leere MODS-Liste mit Hinweis), Leiste am oberen Rand.
2. Ein Rennen starten: Log zeigt `event: map loaded "<Name>"` und `event: game state Solo race`. Ziel/Checkpoints gibt es auf Sunrise noch nicht (steht so im Log).

## 3b. Sunrise mit Ziel-Ereignissen (Entwicklerstand)

Ordner: `C:\Users\osaro\Games\TMSunrise-horizon-next` (Kopie von TMSunrise-horizon-test). Darin: Horizon Host 0.2.0 vom Branch `next` (Commit 46759cc), Legacy Randomizer 0.2.0 aus legacy-randomizer 3348e97 (nutzt echte Rennereignisse, wenn der Host sie liefert), TMX Upload (aus). Die getesteten Ordner bleiben auf 0.1.0.

Neu in Sunrise: Start, Checkpoint, Ziel und Ende des Solo-Rennens kommen aus dem Spiel selbst (statische Adressen, beim Start nur gesetzt, wenn die Bytes genau passen). Respawn und das Ende eines Netzwerk-Rennens gibt es auf Sunrise noch nicht.

1. `TmSunrise.exe` im Ordner starten. Im Log (`horizon\logs\`, neueste Datei) steht: `Sunrise events (...): start on, finish on, checkpoint on, solo race end on`.
2. Eine Solo-Strecke fahren. Im Log je Lauf: `event: race start`, bei jedem Checkpoint `event: checkpoint` (Zeit prüfen: passt sie zur Anzeige im Spiel? Sie ist noch nicht im Spiel bestätigt), im Ziel `event: finish` mit der Zeit, danach `race end`. Insert drücken: der Status zeigt auf dem Endbildschirm "End of race".
3. Neustart mit Entf / Respawn: Start kommt neu, Respawn wird (noch) nicht gemeldet.
4. Legacy Randomizer im Menü öffnen und eine Runde starten: er sollte die Zielankunft jetzt selbst erkennen, ohne dass du etwas drücken musst.
5. Menü (Insert): General, Hotkeys, Log und About haben jetzt Symbole, Knöpfe sind heller als die Karten. TMX Upload steht unter Mods und ist aus.
6. Wenn etwas hängt oder abstürzt: die neueste Log-Datei schicken. Zurück auf den getesteten Stand: einfach wieder TMSunrise-horizon-test benutzen.

Automatischer Kurztest (ohne Anmeldung, 30 s, nur Hauptmenü): alle vier Stellen gesetzt, Randomizer geladen, kein Fehler.

## 4. Forever über TMLoader (experimentell, nur wenn Zeit ist)

Nicht installiert (dein TMLoader-Ordner bleibt unberührt). Zum Testen: `C:\Users\osaro\Downloads\horizon-host\dist\tmloader\Horizon Host` nach `%LOCALAPPDATA%\TMLoader\database\TmForever\products\` kopieren, im TMLoader „Horizon Host“ anhaken, starten. Erwartet: Menü und Leiste, keine Spiel-Hooks, Eingabe wird nie blockiert (Competition Patch). Log: `...\products\Horizon Host\logs\`. Danach wieder abhaken. Mit TMVS + CenterSpeed zusammen: sag mir, ob beide Overlays laufen.

## 5. Forever über TMLoader: Ereignisse (Entwicklerstand)

Nicht installiert, nur zum Nachsehen, falls ich es später teste. Forever liest jetzt Spielzustand, Karte, Start und Ziel nur lesend aus dem Spiel (einmal pro Bild, kein Hook, nichts wird geschrieben). Das Profil `2.12.0` (verschobenes Abbild) geht jetzt auch, nicht nur `2.12.0-compat`. Noch nie im Spiel gelaufen.

Im Log (`...\products\Horizon Host\logs\`, neueste Datei) prüfen:

1. Beim Start: `game: Forever adapter (...): polling only ...`, davor `gamectl: TrackMania Nations Forever (... image base ..., relocated ...)`. Danach `game: reflection offsets: ...` ohne `MISSING`. Steht dort `MISSING` oder `exe not known to gamectl` / `reflection not available`: Log schicken, dann bleiben die Ereignisse aus.
2. Erste Solo-Strecke starten: Zeilen `game: object class CTrackMania ... = app`, `... = challenge`, `... = solo race`, `... = player info`, dann `game: the local player's info is found through race.TMTargetPlayerInfo` (oder `fallback path`). Steht `a race is running but the local player's info was not found`: keine Start-/Ziel-Ereignisse.
3. Pro Strecke: `event: map loaded` (Name und Medaillenzeiten prüfen, uid ist leer, wenn die Strecke nicht vom Randomizer gestartet wurde), `event: race start` beim Losfahren und bei jedem Neustart (Entf), `event: finish` mit Zeit (gleich der Zeit im Spiel?) und `(new best)` (erste Fahrt und jede schnellere: ja, langsamere: nein).
4. Zielbildschirm: Spielzustand `End of race` im Bar/About, danach wieder `Solo race`. Menü: `Main menu`. Editor und Replay-Ansicht: Zustand `Editor` bzw. `Replay`, keine Start-/Ziel-Ereignisse.
5. Online-Rennen: Zustand `Online race`, Solo-Plugins pausieren. Nichts wird geschrieben oder blockiert.
6. Mit Competition Patch geladen: gleiches Verhalten erwartet, Log kontrollieren (keine Hooks, keine Eingabesperre).
7. Randomizer: `play_map_file` startet eine Datei aus dem Hauptmenü (zum ersten Mal auf Forever getestet), `return_to_menu` beendet das Solo-Rennen.

## Horizon Performance (Branch `perf`, Plugin `performance`)

Nur Testkopien (`C:\Users\osaro\Games\TMNF-test`, `TMN-ESWC-test`, `TMS-test`; TMUF optional). Horizon-Build aus `horizon-host-wt-perf\dist` über TM Horizon installieren (Quelle: lokaler Build-Ordner), dann im Horizon-Menü „Horizon Performance“ einschalten (ist neu und daher aus). Was alles bedeutet: `docs/performance.md`. Ergebnisse liegen in `horizon\data\performance\bench\` (Forever: `...\products\Horizon Host\data\performance\bench\`), der Hänger-Log in `...\performance\hangs.log`.

### A. Messen (pro Spiel einmal, ca. 5 min)

1. Spiel starten, Strecke fahren. Fenster „Frame times“ oben rechts: FPS, 1 %/0,1 % low, Ruckler, Grafik.
2. Auf der Plugin-Seite „Count Direct3D calls“ an, eine Minute fahren, wieder aus. Im Horizon-Log stehen alle 30 s `stats:`-Zeilen (Draws, States und wie viele doppelt, Locks, GPU wait, Software-VP-Draws). **Diese Zeilen und die Zeile `d3d: device flags ...` brauche ich von jedem Spiel**, daran entscheide ich, ob der State-Filter überhaupt Sinn hat.

### B. Benchmark: Basis gegen jede Option (pro Spiel, je 3 Läufe)

Gleiche Strecke, gleiche Auflösung, Zählung aus, TMVS aus (oder bei allen Läufen gleich), sonst nichts im Hintergrund.

1. In TM Horizon: Spiel wählen → Leistung → Reiter „Benchmark“ → „Karte wählen“ (eine `.Challenge.Gbx` aus `Tracks\Challenges`, für jedes Spiel eine eigene), Name `baseline`, Läufe 3, „Benchmark starten“. Login wie immer; ab dem Hauptmenü läuft es allein (Karte lädt, Auto steht am Start, 5 s Aufwärmen, 3 × 30 s, zurück ins Menü). Nichts drücken.
2. Spiel beenden. Im Horizon-Menü genau **eine** Option einschalten, Name = Name der Option, wieder 3 Läufe:
   - `limit` – Frame limit auf 10 % unter deinen Basis-FPS (z. B. 300 → 270)
   - `vsync-off` – V-Sync „off“ (nur sinnvoll, wenn das Spiel sonst V-Sync an hat)
   - `filter` – „Skip repeated states“ (nur ohne TMVS-d3d9.dll und ohne Competition Patch wählbar)
   - `finetimer` – Fine timer
   - `nothrottle` – No power throttling
   - `prio` – Priority „above normal“
   - `cores` – CPU cores „performance cores“ (nur bei Intel 12. Gen. oder neuer sinnvoll)
   Danach die Option wieder aus, nächste.
3. Die Vergleichstabelle in TM Horizon zeigt pro Label den Mittelwert und den Unterschied zu `baseline`. Ein Unterschied unter ±3 % ist Rauschen. Mir die Tabelle (Screenshot) und den Ordner `bench` schicken.

Der Benchmark per Hand geht auch: Seite „Benchmark“ → Label, Sekunden, „Start“ (oder Strg+F10).

### C. Hänger beim Start (TMUF über TMLoader, TMNF/TMUF standalone)

1. In TM Horizon bei TMNF und TMUF: Leistung → Reiter „Start“ → „Nächste Starts aufzeichnen“ an (kein Horizon nötig, nur Beobachtung von außen). Wenn Horizon läuft: „Hang log“ ist standardmäßig an.
2. Normal spielen und abwechselnd TMNF und TMUF starten. Wenn ein Start hängt: warten bis es weitergeht (oder 3 min), dann **beide** Dateien schicken: die neueste `startwatch\…txt` aus TM Horizon und `hangs.log`. Zum Vergleich auch einen normalen Start.
3. Gegenprobe, nur wenn es oft hängt: einmal mit `/noscancache` starten (schaltet den Datei-Cache ab, jeder Start liest dann alle Kopfdaten: wird es dann jedes Mal langsam, ist es der Cache).

### D. Replay-Liste „Refresh“

1. Hang log an. Im Spiel Replays → Refresh drücken, warten. `hangs.log` schicken (Dauer, gelesene MB, Funktion).
2. In TM Horizon: Leistung → Reiter „Replays“ (Replay-Archiv): erst nur ansehen, was verschoben würde; wenn ok, archivieren, im Spiel wieder Refresh. Rückgängig geht mit „Letztes Archivieren rückgängig“.

### E. DXVK (optional, experimentell)

Nur TMNF-test ohne TMVS-d3d9.dll im Ordner: in TM Horizon DXVK an, Benchmark `dxvk` 3 Läufe, wieder aus. Competition Patch und Online: nicht geklärt, nur offline testen.

Zurück: alle Optionen aus (Standard), DXVK aus, Replay-Archiv rückgängig, „Optimieren“ hat für jede Änderung „Rückgängig“.

## Entfernen

`Horizon-Uninstall.cmd` im Spielordner (macht aus `dinput8.chain.dll` wieder `dinput8.dll`), danach den Ordner `horizon` löschen. Oder einfach die Testkopie löschen.
