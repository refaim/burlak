<#
.SYNOPSIS
    Reruns the tests that failed in the last ctest run under cdb and prints their crash analysis and stacks.
.DESCRIPTION
    A fast-fail (0xC0000409: a stack cookie, a control-flow-guard violation, a corrupted heap list entry, an
    invalid CRT parameter) leaves no message behind, so CI reruns each failed executable under the Windows SDK
    debugger, which stops on the exception and prints !analyze -v and every thread's stack. Nothing here changes
    the outcome of the ctest step.
.PARAMETER BuildDir
    The ctest build directory that holds Testing/Temporary/LastTestsFailed.log.
.PARAMETER Tests
    Test names to rerun instead of the ones ctest recorded as failed.
.PARAMETER Cdb
    Path to cdb.exe; found under the installed Windows Kits when omitted.
#>
[CmdletBinding()]
param(
    [string] $BuildDir = 'build/coverage',
    [string[]] $Tests,
    [string] $Cdb
)

$ErrorActionPreference = 'Stop'

function Find-Cdb {
    param([string] $Requested)
    if ($Requested) {
        return (Resolve-Path -LiteralPath $Requested).Path
    }
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits", "$env:ProgramFiles\Windows Kits")
    $found = $roots | Where-Object { Test-Path -LiteralPath $_ } | ForEach-Object {
        Get-ChildItem -Path (Join-Path $_ '*\Debuggers\x64\cdb.exe') -ErrorAction SilentlyContinue
    } | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $found) {
        throw 'cdb.exe was not found under the installed Windows Kits'
    }
    return $found.FullName
}

function Get-FailedTestName {
    param([string] $Directory)
    $log = Join-Path $Directory 'Testing\Temporary\LastTestsFailed.log'
    if (-not (Test-Path -LiteralPath $log -PathType Leaf)) {
        return @()
    }
    # Each line is "<index>:<name>".
    return @(Get-Content -LiteralPath $log | ForEach-Object { ($_ -split ':', 2)[1] } | Where-Object { $_ })
}

function Get-TestInventory {
    param([string] $Directory)
    $json = & ctest --test-dir $Directory --show-only=json-v1
    if ($LASTEXITCODE -ne 0) {
        throw 'ctest discovery failed'
    }
    return ($json -join "`n" | ConvertFrom-Json).tests
}

function Get-TestProperty {
    param($Test, [string] $Name)
    $property = @($Test.properties | Where-Object { $_.name -eq $Name }) | Select-Object -First 1
    if ($null -eq $property) {
        return $null
    }
    return $property.value
}

$buildDirectory = (Resolve-Path -LiteralPath $BuildDir).Path
$debugger = Find-Cdb $Cdb
# `powershell -File` passes "core,e2e" as one string, so a comma list is accepted as well as an array.
$names = if ($Tests) { @($Tests -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
else { Get-FailedTestName $buildDirectory }
if ($names.Count -eq 0) {
    Write-Output 'No failed tests recorded; nothing to debug.'
    return
}
$inventory = Get-TestInventory $buildDirectory
# Operating-system frames are named through the public symbol server; the test binaries carry their own PDBs.
$symbolCache = Join-Path ([IO.Path]::GetTempPath()) 'burlak-symbols'
$env:_NT_SYMBOL_PATH = "srv*$symbolCache*https://msdl.microsoft.com/download/symbols;$buildDirectory"

foreach ($name in $names) {
    $test = @($inventory | Where-Object { $_.name -eq $name }) | Select-Object -First 1
    if ($null -eq $test) {
        Write-Output "Test '$name' is not in the ctest inventory; skipped."
        continue
    }
    $command = @($test.command)
    $workingDirectory = Get-TestProperty $test 'WORKING_DIRECTORY'
    if (-not $workingDirectory) {
        $workingDirectory = $buildDirectory
    }
    $environment = @(Get-TestProperty $test 'ENVIRONMENT' | Where-Object { $_ })
    Write-Output "=== cdb: $name ($($command -join ' ')) ==="
    Push-Location $workingDirectory
    try {
        $saved = @{}
        foreach ($entry in $environment) {
            $pair = $entry -split '=', 2
            $saved[$pair[0]] = [Environment]::GetEnvironmentVariable($pair[0])
            [Environment]::SetEnvironmentVariable($pair[0], $pair[1])
        }
        try {
            # -g/-G skip the initial and final breakpoints, -o follows child processes, -lines resolves source lines;
            # the command runs the target, then analyses whatever stopped it and dumps every thread's stack. The
            # target's stderr passes through untouched: under Windows PowerShell a 2>&1 on a native command would
            # turn it into a terminating error.
            & $debugger -g -G -o -lines -c 'g; !analyze -v; ~*kv; q' @command
            Write-Output "=== cdb exit code: $LASTEXITCODE ==="
        }
        finally {
            foreach ($key in $saved.Keys) {
                [Environment]::SetEnvironmentVariable($key, $saved[$key])
            }
        }
    }
    finally {
        Pop-Location
    }
}
