<#
.SYNOPSIS
    Packs a built Burlak.dll into the release archive.

.DESCRIPTION
    Takes build\<Arch>\Burlak.dll from build.ps1, puts it in a Burlak\ folder
    together with dist\ChangeLog, dist\readme_en.txt, dist\readme_ru.txt and
    LICENSE, and zips that as Burlak-<version>-<Arch>.zip in the repository
    root, so the archive unpacks straight into Far's Plugins directory. The
    version comes from src\version.h, not from a tag, so untagged builds are
    still identifiable. The same script runs locally and in CI. Returns the
    archive name without the extension.

.EXAMPLE
    pwsh -File build.ps1 -Arch x64
    pwsh -File package.ps1 -Arch x64        # Burlak-1.2.0-x64.zip
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'arm64')]
    [string] $Arch = 'x64'
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$dll = Join-Path $root "build\$Arch\Burlak.dll"
if (-not (Test-Path $dll)) { throw "$dll not found; run build.ps1 -Arch $Arch first" }

$h = Get-Content (Join-Path $root 'src\version.h') -Raw
$major = [regex]::Match($h, 'BURLAK_VERSION_MAJOR\s+(\d+)').Groups[1].Value
$minor = [regex]::Match($h, 'BURLAK_VERSION_MINOR\s+(\d+)').Groups[1].Value
$patch = [regex]::Match($h, 'BURLAK_VERSION_PATCH\s+(\d+)').Groups[1].Value
$name = "Burlak-$major.$minor.$patch-$Arch"

# A fresh stage per architecture: Compress-Archive would otherwise pick up
# whatever an earlier run left there.
$stage = Join-Path $root "stage\$Arch\Burlak"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $dll $stage
foreach ($doc in 'dist\ChangeLog', 'dist\readme_en.txt', 'dist\readme_ru.txt', 'LICENSE') {
    Copy-Item (Join-Path $root $doc) $stage
}

$zip = Join-Path $root "$name.zip"
Compress-Archive -Path $stage -DestinationPath $zip -Force
Write-Host "packed $Arch : $zip"
$name
