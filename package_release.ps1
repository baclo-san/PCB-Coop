<#
  package_release.ps1 — assemble the end-user beta drop into a single zip.

  Bundles the two files the player drops into their PCB folder (launcher.exe +
  th07_coop.dll) plus the default coop.ini and README. th07.exe is NEVER
  included (copyrighted — the player already has it).

  Run build.ps1 first so build\launcher.exe + build\th07_coop.dll exist.

  Usage:
    powershell -ExecutionPolicy Bypass -File package_release.ps1 [-Version v0.1]
#>
param([string]$Version = "beta")

$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$build = Join-Path $root "build"
$stage = Join-Path $build "release\PCB-Coop-$Version"
$zip   = Join-Path $build "release\PCB-Coop-$Version.zip"

$need = @(
    (Join-Path $build "launcher.exe"),
    (Join-Path $build "th07_coop.dll")
)
foreach ($f in $need) {
    if (-not (Test-Path $f)) { throw "missing $f — run build.ps1 first." }
}

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item (Join-Path $build "launcher.exe")     $stage
Copy-Item (Join-Path $build "th07_coop.dll")    $stage
Copy-Item (Join-Path $root  "release\coop.ini")  $stage
Copy-Item (Join-Path $root  "release\README.txt") $stage

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path "$stage\*" -DestinationPath $zip

Write-Host "Packaged:" -ForegroundColor Green
Get-ChildItem $stage | ForEach-Object { "  {0,-18} {1,8:N0} bytes" -f $_.Name, $_.Length }
Write-Host ""
Write-Host "  -> $zip" -ForegroundColor Yellow
