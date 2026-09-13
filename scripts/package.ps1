<#
.SYNOPSIS
    Packs a Release preset output into Burlak-<version>-<arch>.zip.
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'arm64')]
    [string] $Arch = 'x64'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$dll = Join-Path $root "build\release-$Arch\Burlak.dll"
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    throw "$dll not found; run scripts/build.ps1 -Arch $Arch first"
}

$header = Get-Content -LiteralPath (Join-Path $root 'src\plugin\version.h') -Raw
$major = [regex]::Match($header, 'BURLAK_VERSION_MAJOR\s+(\d+)').Groups[1].Value
$minor = [regex]::Match($header, 'BURLAK_VERSION_MINOR\s+(\d+)').Groups[1].Value
$patch = [regex]::Match($header, 'BURLAK_VERSION_PATCH\s+(\d+)').Groups[1].Value
$name = "Burlak-$major.$minor.$patch-$Arch"

$packageRoot = Join-Path $root "build\package-$Arch"
$stage = Join-Path $packageRoot 'Burlak'
$expectedPrefix = (Join-Path $root 'build') + [IO.Path]::DirectorySeparatorChar
if (-not $packageRoot.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace a package directory outside $expectedPrefix"
}
if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -LiteralPath $dll -Destination $stage
foreach ($document in 'dist\ChangeLog', 'dist\readme_en.txt', 'dist\readme_ru.txt', 'LICENSE') {
    Copy-Item -LiteralPath (Join-Path $root $document) -Destination $stage
}

$zip = Join-Path $root "$name.zip"
Compress-Archive -LiteralPath $stage -DestinationPath $zip -Force
Write-Information "packed $Arch : $zip" -InformationAction Continue
$name
