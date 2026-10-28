param([int]$WaitSeconds = 600, [switch]$AllowExisting)
throw 'Live breakpoint tracing is disabled: the game crashed at the first breakpoint (STATUS_WX86_BREAKPOINT). No attachment will be attempted. Use offline dump inspection only.'
$ErrorActionPreference = 'Stop'
$repoPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$cdbPath = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe'
$gamePath = 'D:\SteamLibrary\steamapps\common\the witcher 2\bin\witcher2.exe'
$commandsPath = Join-Path $PSScriptRoot 'capture-trace.commands.txt'
if ((Get-FileHash -LiteralPath $gamePath).Hash -ne 'CB9FBC9C1DC7AE0AC73C5768E55724CC8E7B78999299F0A2E6E5AB7D74A55FE8') { throw 'Game executable differs from verified build.' }
if (-not (Test-Path -LiteralPath $cdbPath -PathType Leaf)) { throw 'Microsoft x86 CDB is not installed.' }
$armedAt = [DateTime]::UtcNow
$deadline = $armedAt.AddSeconds($WaitSeconds)
Write-Output 'W2PREVIEW ARMED - waiting for normal Steam/Launcher startup.'
while ([DateTime]::UtcNow -lt $deadline) {
    $gameProcesses = @(Get-Process -Name witcher2 -ErrorAction SilentlyContinue)
    if ($gameProcesses.Count -gt 1) { throw 'Multiple game processes found; refusing ambiguous attach.' }
    if ($gameProcesses.Count -eq 1) {
        $gameProcess = $gameProcesses[0]
        if (-not $AllowExisting -and $gameProcess.StartTime.ToUniversalTime() -lt $armedAt) { throw 'Start a new game session after arming the trace.' }
        if ([IO.Path]::GetFullPath($gameProcess.MainModule.FileName) -ine [IO.Path]::GetFullPath($gamePath)) { throw 'Process executable path mismatch.' }
        # Do not open the runtime log: fopen_s holds it exclusively while running.
        # The runtime code signature in CDB, not a log-file read, gates breakpoints.
        if (([DateTime]::UtcNow - $gameProcess.StartTime.ToUniversalTime()).TotalSeconds -ge 10) {
            $traceLog = Join-Path $repoPath ('docs\validation\save-preview-capture-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-UTC.log')
            Write-Output "W2PREVIEW ATTACHING pid=$($gameProcess.Id) log=$traceLog"
            & $cdbPath -p $gameProcess.Id -pd -G -sins -y (Join-Path $repoPath 'build') -logo $traceLog -cf $commandsPath
            exit $LASTEXITCODE
        }
    }
    Start-Sleep -Milliseconds 500
}
throw 'No ready game session appeared before the trace wait expired; no debugger was attached.'
