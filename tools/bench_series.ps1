# Writes bench-request.json for the Horizon Performance benchmark series (docs/performance.md, "Benchmark series").
# The host reads it at the next game start: it switches the Performance module on for that start (it is off by
# default), the series runs by itself and writes <data>\bench\<stamp>_series.json.
#
#   tools/bench_series.ps1 -GamePath C:/path/to/TMN-ESWC-test        ESWC / Sunrise: writes into <game>\horizon\data\performance\
#   tools/bench_series.ps1 -Forever                                  TMNF / TMUF via TMLoader: writes a copy here and prints
#                                                                    the one copy command into the TMLoader product folder
#   tools/bench_series.ps1 -Forever -Install                         ... and copies it there itself
#   options: -Replay <path or name>  -Map <path of a .Challenge.Gbx> (the plugin starts the map itself)
#            -Now (start by the key, whatever is on screen)  -Runs 3  -Warmup 1  -Seconds 20  -Settle 2
#
# Start: replay mode starts by itself as soon as the game reports a replay (ESWC, TMNF, TMUF). Where the game
# cannot report it (Sunrise) or if nothing happens: press the benchmark key (default Ctrl+Shift+B) while the
# replay plays. The line at the top of the screen says what to do and shows the progress.
param(
    [string]$GamePath = "",
    [string]$Replay = "",
    [string]$Map = "",
    [int]$Runs = 3,
    [int]$Warmup = 1,
    [double]$Seconds = 20,
    [double]$Settle = 2,
    [switch]$Now,
    [switch]$Forever,
    [switch]$Install
)
$ErrorActionPreference = "Stop"

$series = [ordered]@{}
if ($Map -ne "") {
    $series.map = $Map
    $series.mode = "map"
} elseif ($Now) {
    $series.mode = "now"
} else {
    $series.mode = "replay"
    if ($Replay -ne "") { $series.replay = $Replay }
}
$series.seconds = $Seconds
$series.settle = $Settle
$series.runs = $Runs
$series.warmup = $Warmup
$request = [ordered]@{ version = 1; label = "series"; series = $series }
$json = ($request | ConvertTo-Json -Depth 6)
$utf8 = New-Object System.Text.UTF8Encoding($false)

$windows = 7 * ($Runs + $Warmup)
$minutes = ($windows * ($Seconds + $Settle)) / 60
$replayLen = [int][Math]::Ceiling(2 * $Seconds)
function Show-Steps {
    Write-Host ""
    Write-Host ("Dauer etwa {0:N0} Minuten ({1} Fenster zu je {2} s plus {3} s Einschwingen)." -f $minutes, $windows, $Seconds, $Settle)
    if ($Map -ne "") {
        Write-Host "Map-Modus: Spiel starten und nicht anfassen. Die Strecke startet von selbst."
    } elseif ($Now) {
        Write-Host "Jetzt-Modus: Spiel starten, die Szene oeffnen, dann Strg+Umschalt+B druecken."
    } else {
        Write-Host ("Replay-Modus: Spiel starten, ein Replay oeffnen (mind. {0} s lang, es darf in Schleife laufen)." -f $replayLen)
        Write-Host "  Die Serie startet von selbst. Startet sie nicht (Sunrise): Strg+Umschalt+B druecken, solange das Replay laeuft."
    }
    Write-Host "  Oben im Bild steht, was zu tun ist, und der Fortschritt (Benchmark x/21 ...). Dieselbe Taste bricht ab."
    Write-Host "  Das Performance-Modul muss nicht eingeschaltet sein: der Auftrag schaltet es fuer diesen Start ein."
}

$isForever = $Forever.IsPresent
if (-not $isForever -and $GamePath -ne "") {
    $game = (Resolve-Path -LiteralPath $GamePath).Path.TrimEnd('\')
    $legacyHost = Join-Path $game "horizon\host.dll"
    $isForever = -not (Test-Path -LiteralPath $legacyHost) -and ((Test-Path -LiteralPath (Join-Path $game "TmForever.exe")) -or (Test-Path -LiteralPath (Join-Path $game "TmForeverLauncher.exe")))
}
if (-not $isForever -and $GamePath -eq "") {
    Write-Error "-GamePath <Spielordner> (ESWC/Sunrise) oder -Forever (TMNF/TMUF ueber TMLoader) angeben."
    exit 1
}

if ($isForever) {
    # Forever host via TMLoader: data root = the product folder above the version folder (src/host/main.cpp),
    # the same for TMNF and TMUF.
    $data = Join-Path $env:LOCALAPPDATA "TMLoader\database\TmForever\products\Horizon Host\data\performance"
    $target = Join-Path $data "bench-request.json"
    $staging = Join-Path (Get-Location) "bench-request.json"
    [System.IO.File]::WriteAllText($staging, $json, $utf8)
    Write-Host "Auftrag (TMNF/TMUF): $staging"
    Write-Host $json
    if ($Install) {
        New-Item -ItemType Directory -Force -Path $data | Out-Null
        Copy-Item -LiteralPath $staging -Destination $target -Force
        Write-Host ""
        Write-Host "Kopiert nach: $target"
    } else {
        Write-Host ""
        Write-Host "Diesen Befehl ausfuehren (legt den Ordner an und kopiert den Auftrag dorthin, wo der Forever-Host liest):"
        Write-Host ""
        Write-Host ("  New-Item -ItemType Directory -Force -Path '{0}' | Out-Null; Copy-Item -LiteralPath '{1}' -Destination '{2}' -Force" -f $data, $staging, $target)
        Write-Host ""
        Write-Host "(oder dieses Skript mit -Install aufrufen)"
    }
    Write-Host ""
    Write-Host "Dann TMNF/TMUF ueber TMLoader starten, Horizon Host angehakt."
    Show-Steps
    Write-Host ("Ergebnis: {0}\bench\<Zeit>_series.json  ->  python tools/bench_report.py <Datei>" -f $data)
    exit 0
}

$dataDir = Join-Path $game "horizon\data\performance"
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
$target = Join-Path $dataDir "bench-request.json"
[System.IO.File]::WriteAllText($target, $json, $utf8)
Write-Host "Geschrieben: $target"
Write-Host $json
Show-Steps
Write-Host "Ergebnis: $dataDir\bench\<Zeit>_series.json  ->  python tools/bench_report.py <Datei>"
