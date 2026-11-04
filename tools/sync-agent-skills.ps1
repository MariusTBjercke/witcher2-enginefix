<#
.SYNOPSIS
  Mirror the project's agent skills from .agents/skills into .claude/skills.

.DESCRIPTION
  .agents/skills is the source: Codex, DSH and other harnesses read it. Claude
  Code reads only .claude/skills, so that tree is a plain copy, tracked in git
  like the source. Edit .agents/skills and run this script; never edit the copy.

  Files are copied byte for byte, and a skill or file that no longer exists in
  the source is removed from the copy.

  -Check changes nothing: it lists every difference and exits 1 if there is
  one, so a review can tell whether the two trees agree.
#>
param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root '.agents\skills'
$target = Join-Path $root '.claude\skills'

function Get-RelativeFiles([string]$base) {
    if (-not (Test-Path -LiteralPath $base)) {
        return @()
    }
    Get-ChildItem -LiteralPath $base -Recurse -File |
        ForEach-Object { $_.FullName.Substring($base.Length + 1) }
}

function Test-SameBytes([string]$a, [string]$b) {
    if (-not (Test-Path -LiteralPath $b)) {
        return $false
    }
    $left = [System.IO.File]::ReadAllBytes($a)
    $right = [System.IO.File]::ReadAllBytes($b)
    if ($left.Length -ne $right.Length) {
        return $false
    }
    for ($i = 0; $i -lt $left.Length; $i++) {
        if ($left[$i] -ne $right[$i]) {
            return $false
        }
    }
    return $true
}

$sourceFiles = @(Get-RelativeFiles $source)
$targetFiles = @(Get-RelativeFiles $target)
$differences = 0

foreach ($file in $sourceFiles) {
    $from = Join-Path $source $file
    $to = Join-Path $target $file
    if (Test-SameBytes $from $to) {
        continue
    }
    $differences++
    if ($Check) {
        Write-Output "differs: $file"
        continue
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
    Copy-Item -LiteralPath $from -Destination $to -Force
    Write-Output "copied:  $file"
}

foreach ($file in $targetFiles) {
    if ($sourceFiles -contains $file) {
        continue
    }
    $differences++
    if ($Check) {
        Write-Output "stale:   $file"
        continue
    }
    Remove-Item -LiteralPath (Join-Path $target $file) -Force
    Write-Output "removed: $file"
}

if (-not $Check -and (Test-Path -LiteralPath $target)) {
    # Folders left empty by a removed skill.
    Get-ChildItem -LiteralPath $target -Recurse -Directory |
        Sort-Object { $_.FullName.Length } -Descending |
        Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force) } |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
}

if ($Check) {
    if ($differences -gt 0) {
        Write-Output "$differences difference(s) between .agents\skills and .claude\skills; run tools\sync-agent-skills.ps1"
        exit 1
    }
    Write-Output 'in sync'
}
elseif ($differences -eq 0) {
    Write-Output 'already in sync'
}
exit 0
