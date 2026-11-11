# Writes bench-request.json for the Horizon Performance benchmark series (docs/performance.md, "Benchmark series").
# The plugin reads it at the next game start, runs the series by itself and writes <data>\bench\<stamp>_series.json.
#
#   tools/bench_series.ps1 -GamePath C:/path/to/TMN-ESWC-test                 (ESWC / Sunrise: writes the file)
#   tools/bench_series.ps1 -GamePath C:/path/to/TmForever -Forever             (Forever: only prints path and content)
#   options: -Replay <path or name>  -Map <path of a .Challenge.Gbx> (the plugin starts the map itself)
#            -Runs 3  -Warmup 1  -Seconds 20  -Settle 2
#
# Forever: the file belongs into the TMLoader product data folder; this script does not write into
# %LOCALAPPDATA%\TMLoader, it prints where to put the file.
param(
    [Parameter(Mandatory = $true)][string]$GamePath,
    [string]$Replay = "",
    [string]$Map = "",
    [int]$Runs = 3,
    [int]$Warmup = 1,
    [double]$Seconds = 20,
    [double]$Settle = 2,
    [switch]$Forever
)
$ErrorActionPreference = "Stop"

$series = [ordered]@{}
if ($Map -ne "") {
    $series.map = $Map
    $series.mode = "map"
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

$game = (Resolve-Path -LiteralPath $GamePath).Path.TrimEnd('\')
$legacyHost = Join-Path $game "horizon\host.dll"
$isForever = $Forever.IsPresent -or (-not (Test-Path -LiteralPath $legacyHost) -and ((Test-Path -LiteralPath (Join-Path $game "TmForever.exe")) -or (Test-Path -LiteralPath (Join-Path $game "TmForeverLauncher.exe"))))

if ($isForever) {
    $data = Join-Path $env:LOCALAPPDATA "TMLoader\database\TmForever\products\Horizon Host\data\performance"
    Write-Host "Forever: nicht geschrieben. Lege diese Datei an:"
    Write-Host "  $data\bench-request.json"
    Write-Host ""
    Write-Host "Inhalt:"
    Write-Host $json
    Write-Host ""
    Write-Host "Danach das Spiel ueber TMLoader starten (Horizon Host angehakt) und nicht anfassen."
    exit 0
}

if (-not (Test-Path -LiteralPath $legacyHost)) {
    Write-Error "Kein horizon\host.dll in $game. Das ist keine Testkopie mit Horizon (ESWC/Sunrise)?"
    exit 1
}
$dataDir = Join-Path $game "horizon\data\performance"
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
$target = Join-Path $dataDir "bench-request.json"
[System.IO.File]::WriteAllText($target, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "Geschrieben: $target"
Write-Host $json
Write-Host ""
$windows = 7 * ($Runs + $Warmup)
Write-Host ("Jetzt das Spiel starten und nicht anfassen. Dauer etwa {0:N0} Minuten ({1} Fenster zu je {2} s plus {3} s Einschwingen)." -f (($windows * ($Seconds + $Settle)) / 60), $windows, $Seconds, $Settle)
if ($Map -eq "") {
    Write-Host "Replay-Modus: Das Spiel kann ein Replay nicht selbst oeffnen. Einmal ein Replay oeffnen (im Spiel erscheint oben, welches), dann laeuft die Serie von allein."
}
Write-Host "Ergebnis: $dataDir\bench\<Zeit>_series.json  ->  python tools/bench_report.py <Datei>"
