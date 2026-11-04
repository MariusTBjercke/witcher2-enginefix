<#
.SYNOPSIS
  Adds or removes a fake user content package for an in-game test of the script patches.

.DESCRIPTION
  The game compiles a script from the last mounted user content package that has it, before
  base_scripts.dzip (docs/fixes/dll-script-patches.md, "User content packages"). This makes
  such a package: Documents\Witcher 2\UserContent\w2ef_test\ with an empty pack0.dzip and a
  scripts.dzip holding the install's own guiutils.ws and player.ws, each with one comment line
  added, and Documents\Witcher 2\Config\UserContent.ini mounting it.

  With a DLL that does not know about user content the game should stop at "Script compilation
  error" naming W2EF_UseMedallion in extendedmovable.ws. With one that does, it starts, and
  witcher2_enginefix.log names the package and redirects its scripts.dzip.

  -Remove deletes the package, and the ini only if it is still the one this script wrote. An ini
  that was there before is never overwritten.

  Needs build\sptest.exe (tools\patchtest\build.bat). Run with the game closed.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\usercontent-test.ps1 -Game "<game>"
  powershell -ExecutionPolicy Bypass -File tools\usercontent-test.ps1 -Remove
#>
param(
    [string]$Game,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$sptest = Join-Path $repo 'build\sptest.exe'
$docs = [Environment]::GetFolderPath('MyDocuments')
$w2 = Join-Path $docs 'Witcher 2'
$pkg = Join-Path $w2 'UserContent\w2ef_test'
$ini = Join-Path $w2 'Config\UserContent.ini'
$iniText = "[Packages]`r`nMount=w2ef_test`r`n"

function Test-OurIni {
    if (-not (Test-Path -LiteralPath $ini)) { return $false }
    $bytes = [IO.File]::ReadAllBytes($ini)
    return [Text.Encoding]::ASCII.GetString($bytes) -eq $iniText
}

if ($Remove) {
    if (Test-Path -LiteralPath $pkg) {
        Remove-Item -LiteralPath $pkg -Recurse -Force
        Write-Output "removed $pkg"
    }
    $uc = Split-Path -Parent $pkg
    if ((Test-Path -LiteralPath $uc) -and -not (Get-ChildItem -LiteralPath $uc -Force)) {
        Remove-Item -LiteralPath $uc -Force
        Write-Output "removed the empty $uc"
    }
    if (Test-OurIni) {
        Remove-Item -LiteralPath $ini -Force
        Write-Output "removed $ini"
    } elseif (Test-Path -LiteralPath $ini) {
        Write-Output "left $ini as it is: it is not the one this script wrote"
    }
    exit 0
}

if (-not $Game) { throw 'Give -Game "<game>" (the folder that holds bin and CookedPC).' }
if (-not (Test-Path -LiteralPath $sptest)) { throw "Build $sptest first: tools\patchtest\build.bat" }
if ((Test-Path -LiteralPath $ini) -and -not (Test-OurIni)) {
    throw "$ini already exists and is not from this script; not touching it."
}
$scripts = Join-Path $Game 'CookedPC\base_scripts.dzip'
if (-not (Test-Path -LiteralPath $scripts)) { throw "No $scripts" }

New-Item -ItemType Directory -Force -Path $pkg | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ini) | Out-Null
$work = Join-Path $pkg 'src'
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
    $entries = @()
    foreach ($e in @('game\gui\guiutils.ws', 'game\player\player.ws')) {
        $file = Join-Path $work (Split-Path -Leaf $e)
        & $sptest extract $scripts $e $file
        if ($LASTEXITCODE -ne 0) { throw "extract $e failed" }
        $body = [IO.File]::ReadAllBytes($file)
        $head = [Text.Encoding]::ASCII.GetBytes("// w2ef user content test package`r`n")
        [IO.File]::WriteAllBytes($file, [byte[]]($head + $body))
        $entries += "$e=$file"
    }
    & $sptest pack (Join-Path $pkg 'scripts.dzip') @entries
    if ($LASTEXITCODE -ne 0) { throw 'pack scripts.dzip failed' }
    & $sptest pack (Join-Path $pkg 'pack0.dzip')
    if ($LASTEXITCODE -ne 0) { throw 'pack pack0.dzip failed' }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
[IO.File]::WriteAllBytes($ini, [Text.Encoding]::ASCII.GetBytes($iniText))
Write-Output "package: $pkg"
Write-Output "ini:     $ini"
Write-Output 'Remove it again with: tools\usercontent-test.ps1 -Remove'
