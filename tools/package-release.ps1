# Builds dist\Witcher2EngineFix-<version>.zip, laid out to extract straight into the game folder
# (bin\dinput8.dll, bin\witcher2_enginefix.ini, README_Witcher2EngineFix.txt,
# LICENSE_Witcher2EngineFix.txt, CookedPC\globals\... interior maps, the opt-in
# Witcher2EngineFix_Optional\ScriptPatches patcher). Runs build.bat first unless -NoBuild. Entries use "/" separators so
# the zip also extracts correctly outside Windows (PowerShell 5.1's ZipFile writes "\").
param([string]$Version = "", [switch]$NoBuild)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $Version) {
    $Version = (Select-String -Path (Join-Path $root "CHANGELOG.md") -Pattern '^## (\d+\.\d+\.\d+)' |
        Select-Object -First 1).Matches[0].Groups[1].Value
}
$src = Get-Content (Join-Path $root "src\enginefix.c") -Raw
if ($src -notmatch "#define ENGINEFIX_VERSION\s+`"$([regex]::Escape($Version))`"") {
    throw "ENGINEFIX_VERSION in src\enginefix.c does not match $Version from CHANGELOG.md"
}

if (-not $NoBuild) {
    & cmd /c "`"$root\build.bat`""
    if ($LASTEXITCODE -ne 0) { throw "build.bat failed" }
}

# Published path in the zip -> source path in the repo.
$sources = [ordered]@{
    "README_Witcher2EngineFix.txt"  = "docs\README.txt"
    "LICENSE_Witcher2EngineFix.txt" = "LICENSE"
    "bin/dinput8.dll"               = "build\dinput8.dll"
    "bin/witcher2_enginefix.ini"    = "dist-files\witcher2_enginefix.ini"
    # Opt-in: nothing changes until the player runs Install.bat (docs/fixes/menu-buttons.md,
    # docs/fixes/shop-book-read-marker.md).
    "Witcher2EngineFix_Optional/ScriptPatches/ScriptPatches.ps1" = "tools\script-patches\ScriptPatches.ps1"
    "Witcher2EngineFix_Optional/ScriptPatches/Install.bat"       = "tools\script-patches\Install.bat"
    "Witcher2EngineFix_Optional/ScriptPatches/Uninstall.bat"     = "tools\script-patches\Uninstall.bat"
}

# Interior maps are generated from the game's own pack0.dzip by tools\build-interior-maps.py
# (game assets, never committed). Refuse to package without them.
$maps = Join-Path $root "build\interior-maps"
if (-not (Test-Path (Join-Path $maps "CookedPC\globals\map_scales.csv"))) {
    throw "build\interior-maps is missing: run python tools\build-interior-maps.py `"<game>\CookedPC\pack0.dzip`" first"
}
foreach ($f in Get-ChildItem $maps -Recurse -File | Sort-Object FullName) {
    $sources[$f.FullName.Substring($maps.Length + 1).Replace('\', '/')] = $f.FullName.Substring($root.Length + 1)
}

$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$zipPath = Join-Path $dist "Witcher2EngineFix-$Version.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath }

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($entry in $sources.GetEnumerator()) {
        $path = Join-Path $root $entry.Value
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $path, $entry.Key)
    }
}
finally { $zip.Dispose() }

Write-Output "Built $zipPath"
