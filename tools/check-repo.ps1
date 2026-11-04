<#
.SYNOPSIS
  The checks a CI would run, for a repository that cannot have one.

.DESCRIPTION
  Every behaviour test needs the game, so this covers what can be checked here: skill copies,
  versions, the address table, the docs index, and what must not be in tracked files. With -Game
  it also builds the offline harness and runs it against that install.

  Exits 1 if anything fails.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\check-repo.ps1 -Game "<game>"
#>
param(
    [string]$Game
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
$failures = 0

function Report([string]$name, [string[]]$problems) {
    if ($problems.Count -eq 0) {
        Write-Output "ok    $name"
        return
    }
    $script:failures++
    Write-Output "FAIL  $name"
    $problems | ForEach-Object { Write-Output "      $_" }
}

# Skill copies
$sync = @(& powershell -NoProfile -ExecutionPolicy Bypass -File tools\sync-agent-skills.ps1 -Check)
Report 'skills in sync' $(if ($LASTEXITCODE -ne 0) { $sync } else { @() })

# Versions
$changelog = (Select-String -Path CHANGELOG.md -Pattern '^## (\d+\.\d+\.\d+)' |
    Select-Object -First 1).Matches[0].Groups[1].Value
$header = Get-Content src\version.h -Raw
$versionProblems = @()
if ($header -notmatch "#define ENGINEFIX_VERSION\s+`"$([regex]::Escape($changelog))`"") {
    $versionProblems += "ENGINEFIX_VERSION differs from CHANGELOG.md ($changelog)"
}
if ($header -notmatch "#define ENGINEFIX_VERSION_BIN\s+$($changelog.Replace('.', ',')),0\b") {
    $versionProblems += "ENGINEFIX_VERSION_BIN differs from CHANGELOG.md ($changelog)"
}
Report 'versions match' $versionProblems

# Address table
$addresses = @(& python -I tools/address-table.py --check 2>&1 | ForEach-Object { "$_" })
Report 'address table' $(if ($LASTEXITCODE -ne 0) { $addresses } else { @() })

# Docs index
$index = Get-Content docs\fixes\README.md -Raw
$unlisted = @(Get-ChildItem docs\fixes -Filter *.md |
    Where-Object { $_.Name -ne 'README.md' -and $index -notmatch [regex]::Escape("($($_.Name))") } |
    ForEach-Object { "$($_.Name) is not in docs/fixes/README.md" })
Report 'fixes index' $unlisted

# Tracked files
$tracked = @(& git ls-files)
$forbidden = @($tracked | Where-Object {
    $_ -match '^docs/(plans|local)/' -or $_ -match '\.(exe|dll|dzip|dds|w2scripts|sav|dmp)$'
} | ForEach-Object { "$_ should not be tracked" })
Report 'nothing forbidden tracked' $forbidden

# Machine paths and pointers to personal notes. AGENTS.md and the conventions describe these
# folders, the skills use them, and two tools name an example install on purpose.
$allowed = '^(AGENTS\.md|docs/conventions\.md|\.gitignore|\.agents/|\.claude/|tools/check-repo\.ps1|tools/savepreviewprobe/wait-for-capture-trace\.ps1)'
$patterns = @(
    'C:[\\/]Users[\\/]',
    'SteamLibrary',
    'userdata[\\/][0-9]{7,}',
    '7656119[0-9]{10}',
    'docs/plans/',
    'docs/local/'
)
$leaks = @()
foreach ($pattern in $patterns) {
    $hits = @(& git grep -n -I -E -e $pattern 2>$null)
    $leaks += @($hits | Where-Object { ($_ -split ':')[0] -notmatch $allowed })
}
Report 'no machine paths or note pointers' @($leaks | ForEach-Object { $_.Substring(0, [Math]::Min(110, $_.Length)) })

# Offline harness
if ($Game) {
    & cmd /c "`"$root\tools\patchtest\build.bat`"" | Out-Null
    $tests = @(& build\sptest.exe test --game $Game)
    $last = $tests | Select-Object -Last 1
    Report "sptest on $Game" $(if ($last -ne '0 failures') { $tests | Where-Object { $_ -notmatch '^PASS' } } else { @() })
}
else {
    Write-Output 'skip  sptest (no -Game)'
}

if ($failures -gt 0) {
    Write-Output "$failures check(s) failed"
    exit 1
}
Write-Output 'all checks passed'
exit 0
