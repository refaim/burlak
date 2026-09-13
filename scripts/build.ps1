<#
.SYNOPSIS
    Builds the requested Release Burlak.dll with its CMake preset.
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'arm64')]
    [string] $Arch = 'x64'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$preset = "release-$Arch"

Push-Location $root
try {
    & cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    & cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed ($LASTEXITCODE)" }
}
finally {
    Pop-Location
}

$dll = Join-Path $root "build\$preset\Burlak.dll"
$version = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
Write-Output "built $Arch : $dll  (v$version)"
