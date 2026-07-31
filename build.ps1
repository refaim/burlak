<#
.SYNOPSIS
    Builds Burlak.dll.

.DESCRIPTION
    The same script runs locally and in CI, so the two cannot drift apart.
    Locally it finds the MSVC toolchain through vcvars; under GitHub Actions the
    environment is already set up by msvc-dev-cmd, so pass -SkipVcvars.

.EXAMPLE
    pwsh -File build.ps1                      # x64, release
    pwsh -File build.ps1 -Arch x86
    pwsh -File build.ps1 -SkipVcvars          # CI: toolchain already on PATH
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')]
    [string] $Arch = 'x64',

    [switch] $SkipVcvars,

    # Far's plugin headers ship with the repo so CI needs nothing installed.
    [string] $SdkPath
)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
if (-not $SdkPath) { $SdkPath = Join-Path $root 'sdk' }
$src = Join-Path $root 'src'
$out = Join-Path $root "build\$Arch"

New-Item -ItemType Directory -Force -Path $out | Out-Null

if (-not $SkipVcvars) {
    $vcvars = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars$(if ($Arch -eq 'x64') { '64' } else { '32' }).bat"
    if (-not (Test-Path $vcvars)) {
        # Fall back to any edition the machine happens to have.
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($install) {
                $vcvars = Join-Path $install "VC\Auxiliary\Build\vcvars$(if ($Arch -eq 'x64') { '64' } else { '32' }).bat"
            }
        }
    }
    if (-not (Test-Path $vcvars)) { throw "vcvars not found; install the VS Build Tools C++ workload" }

    # Replay the toolchain environment into this session.
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

Push-Location $out
try {
    & rc.exe /nologo /I "$src" /fo burlak.res (Join-Path $src 'burlak.rc')
    if ($LASTEXITCODE -ne 0) { throw "rc failed ($LASTEXITCODE)" }

    & cl.exe /nologo /c /EHsc /W4 /WX /O2 /MT /GS /permissive- /DUNICODE /D_UNICODE `
        /I "$SdkPath" /I "$src" (Join-Path $src 'burlak.cpp')
    if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

    # "/DEF:$path" has to be a single token; /DEF:(...) would parse as two.
    $def = Join-Path $src 'burlak.def'
    & link.exe /nologo /DLL /OUT:Burlak.dll "/DEF:$def" /DEBUG /OPT:REF /OPT:ICF `
        burlak.obj burlak.res user32.lib ole32.lib shell32.lib kernel32.lib
    if ($LASTEXITCODE -ne 0) { throw "link failed ($LASTEXITCODE)" }
}
finally { Pop-Location }

$dll = Join-Path $out 'Burlak.dll'
$version = (Get-Item $dll).VersionInfo.FileVersion
Write-Host "built $Arch : $dll  (v$version)"
