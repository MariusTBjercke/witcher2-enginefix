# Frame-time capture of witcher2.exe with Intel PresentMon, started from inside the game.
#
#   powershell -ExecutionPolicy Bypass -File tools\frametime\capture.ps1 -Label baseline
#
# Ctrl+F11 in the game starts one recording of -Seconds seconds; press it again for the next run.
# A beep marks the start and every -SegmentSeconds, two beeps the end, so the three parts of the
# test (stand still, turn, walk) line up with tools\frametime\analyze.py.
# Output: build\frametime\<Label>-<n>.csv (build\ is git-ignored). Stop with -Stop.
#
# PresentMon only reads Windows' event tracing; it does not touch the game. It needs admin or
# membership of "Performance Log Users". Get it from github.com/GameTechDev/PresentMon/releases
# and put the console exe in tools\presentmon\ (git-ignored).
param(
    [string]$Label = "run",
    [int]$Seconds = 60,
    [int]$SegmentSeconds = 20,
    [string]$Hotkey = "CONTROL+F11",
    [switch]$Stop
)

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pm = Get-ChildItem "$root\tools\presentmon\PresentMon-*-x64.exe" | Select-Object -First 1
if (-not $pm) { throw "PresentMon console exe not found in tools\presentmon\" }
$session = "w2frametime"

if ($Stop) {
    & $pm.FullName --session_name $session --terminate_existing_session | Out-Null
    return
}

$outDir = "$root\build\frametime"
New-Item -ItemType Directory -Force $outDir | Out-Null
$before = @(Get-ChildItem $outDir -Filter "$Label*.csv" | ForEach-Object { $_.FullName })

$proc = Start-Process -FilePath $pm.FullName -WindowStyle Hidden -PassThru -ArgumentList @(
    "--process_name", "witcher2.exe",
    "--output_file", "`"$outDir\$Label.csv`"",
    "--hotkey", $Hotkey,
    "--timed", $Seconds,
    "--scroll_indicator",
    "--no_console_stats",
    "--session_name", $session,
    "--stop_existing_session"
)
"PresentMon pid $($proc.Id): press $Hotkey in the game to record $Seconds s into $outDir"

# Beeps follow the first sight of each new CSV, so they are at most 200 ms late.
$seen = @{}
$before | ForEach-Object { $seen[$_] = $true }
while (-not $proc.HasExited) {
    Start-Sleep -Milliseconds 200
    foreach ($f in Get-ChildItem $outDir -Filter "$Label*.csv") {
        if ($seen.ContainsKey($f.FullName)) { continue }
        $seen[$f.FullName] = $true
        "recording: $($f.Name)"
        [Console]::Beep(880, 200)
        for ($t = $SegmentSeconds; $t -lt $Seconds; $t += $SegmentSeconds) {
            Start-Sleep -Seconds $SegmentSeconds
            [Console]::Beep(660, 200)
        }
        Start-Sleep -Seconds ($Seconds - [math]::Floor(($Seconds - 1) / $SegmentSeconds) * $SegmentSeconds)
        [Console]::Beep(880, 150); Start-Sleep -Milliseconds 120; [Console]::Beep(880, 150)
        "done: $($f.Name)"
    }
}
"PresentMon exited with code $($proc.ExitCode)"
