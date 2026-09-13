[CmdletBinding()]
param(
    [string] $LlvmDir
)

$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param([string] $Command, [string[]] $Arguments)

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

function Find-LlvmDirectory {
    param([string] $RequestedDirectory)
    if ($RequestedDirectory) {
        return (Resolve-Path -LiteralPath $RequestedDirectory).Path
    }
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\x64\bin"
    )
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installations = @(& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang `
            -property installationPath)
        $candidates += @($installations | Where-Object { $_ } | ForEach-Object {
                Join-Path $_ 'VC\Tools\Llvm\x64\bin'
            })
    }
    $found = $candidates | Where-Object {
        Test-Path -LiteralPath (Join-Path $_ 'llvm-cov.exe') -PathType Leaf
    } | Select-Object -First 1
    if (-not $found) {
        throw 'LLVM tools were not found; pass -LlvmDir'
    }
    return [IO.Path]::GetFullPath($found)
}

function ConvertTo-NormalizedPath {
    param([string] $Path)
    return [IO.Path]::GetFullPath($Path).Replace('/', [IO.Path]::DirectorySeparatorChar)
}

function Assert-BurlakProfile {
    param(
        [IO.FileInfo[]] $ProfileFiles,
        [string] $Plugin,
        [string] $ExportsSource,
        [string] $LlvmProfdata,
        [string] $LlvmCov,
        [string] $WorkDirectory
    )

    $pattern = [regex]'^burlak-e2e-(\d+)-([^.]+)\.profraw$'
    $pairs = @($ProfileFiles | ForEach-Object {
            $match = $pattern.Match($_.Name)
            if ($match.Success) {
                [pscustomobject]@{ ProcessId = $match.Groups[1].Value; Module = $match.Groups[2].Value; File = $_ }
            }
        } | Group-Object -Property ProcessId | Where-Object {
            @($_.Group | ForEach-Object { $_.Module } | Sort-Object -Unique).Count -ge 2
        })
    if ($pairs.Count -eq 0) {
        throw 'Burlak profile check failed: the uniquely prefixed e2e process did not write separate executable and DLL profiles.'
    }

    $probe = Join-Path $WorkDirectory 'burlak-profile-check.profdata'
    foreach ($pair in $pairs) {
        $files = @($pair.Group | ForEach-Object { $_.File.FullName })
        Invoke-Checked $LlvmProfdata (@('merge', '-sparse') + $files + @('-o', $probe))
        $json = & $LlvmCov export -summary-only $Plugin "-instr-profile=$probe"
        if ($LASTEXITCODE -ne 0) {
            throw "llvm-cov export on $Plugin failed with exit code $LASTEXITCODE"
        }
        $exports = ($json -join "`n" | ConvertFrom-Json).data[0].files | Where-Object {
            (ConvertTo-NormalizedPath $_.filename) -eq $ExportsSource
        }
        if ($exports -and $exports.summary.functions.covered -gt 0) {
            Write-Output "Burlak DLL profile check passed: e2e process $($pair.Name) wrote $($files.Count) module profiles; Exports.cpp executed $($exports.summary.functions.covered)/$($exports.summary.functions.count) functions."
            return
        }
    }
    throw 'Burlak profile check failed: the uniquely prefixed e2e profiles contain no executed Exports.cpp functions from Burlak.dll.'
}

$repository = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDirectory = Join-Path $repository 'build\coverage'
$llvmDirectory = Find-LlvmDirectory $LlvmDir
$llvmProfdata = Join-Path $llvmDirectory 'llvm-profdata.exe'
$llvmCov = Join-Path $llvmDirectory 'llvm-cov.exe'
$hadProfile = Test-Path -LiteralPath 'Env:LLVM_PROFILE_FILE'
$oldProfile = $env:LLVM_PROFILE_FILE
$sourceRoot = ConvertTo-NormalizedPath (Join-Path $repository 'src')
$sourcePrefix = $sourceRoot.TrimEnd('\') + '\'
$inventoryPath = Join-Path $PSScriptRoot 'coverage-sources.txt'
$entries = @(Get-Content -LiteralPath $inventoryPath | ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and -not $_.StartsWith('#') })
$inventorySet = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$classified = @($entries | ForEach-Object {
        $match = [regex]::Match($_, '^(executable|declarations-only)\s+(.+)$')
        if (-not $match.Success) {
            throw "Invalid coverage inventory line: $_"
        }
        $classification = $match.Groups[1].Value
        $relativePath = $match.Groups[2].Value.Trim()
        $candidate = ConvertTo-NormalizedPath (Join-Path $repository $relativePath)
        if (-not $candidate.StartsWith($sourcePrefix, [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            throw "Invalid coverage inventory entry: $relativePath"
        }
        if (-not $inventorySet.Add($candidate)) {
            throw "Coverage inventory contains duplicate path: $relativePath"
        }
        [pscustomobject]@{
            Classification = $classification
            RelativePath = $relativePath.Replace('\', '/')
            FullPath = $candidate
        }
    })
$unlistedSources = @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Force | Where-Object {
        -not $inventorySet.Contains((ConvertTo-NormalizedPath $_.FullName))
    } | ForEach-Object { $_.FullName.Substring($repository.Length + 1).Replace('\', '/') })
if ($unlistedSources.Count -ne 0) {
    throw "Coverage inventory is incomplete:`n  $($unlistedSources -join "`n  ")"
}
$executableSources = @($classified | Where-Object { $_.Classification -eq 'executable' })
$declarationSources = @($classified | Where-Object { $_.Classification -eq 'declarations-only' })

Push-Location $repository
try {
    Invoke-Checked 'cmake' @(
        '--preset', 'coverage',
        "-DCMAKE_CXX_COMPILER=$(Join-Path $llvmDirectory 'clang-cl.exe')",
        "-DCMAKE_RC_COMPILER=$(Join-Path $llvmDirectory 'llvm-rc.exe')",
        "-DCMAKE_LINKER=$(Join-Path $llvmDirectory 'lld-link.exe')"
    )
    Invoke-Checked 'cmake' @('--build', '--preset', 'coverage')

    Get-ChildItem -LiteralPath $buildDirectory -Filter 'burlak-*.profraw' -File -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
    $inventoryJson = & ctest --test-dir $buildDirectory --show-only=json-v1
    if ($LASTEXITCODE -ne 0) {
        throw 'CTest discovery failed'
    }
    $env:LLVM_PROFILE_FILE = Join-Path $buildDirectory 'burlak-%p-%m.profraw'
    Invoke-Checked 'ctest' @('--preset', 'coverage', '--output-on-failure')

    $profiles = @(Get-ChildItem -LiteralPath $buildDirectory -Filter 'burlak-*.profraw' -File)
    if ($profiles.Count -eq 0) {
        throw "No raw coverage profiles were produced in $buildDirectory"
    }
    Write-Output "Raw profiles produced: $($profiles.Count)"
    $profiles | ForEach-Object { Write-Output "  $($_.Name) ($($_.Length) bytes)" }

    $profileData = Join-Path $buildDirectory 'coverage.profdata'
    Invoke-Checked $llvmProfdata (@('merge', '-sparse') + $profiles.FullName + @('-o', $profileData))

    $inventory = $inventoryJson | ConvertFrom-Json
    $buildPrefix = (ConvertTo-NormalizedPath $buildDirectory).TrimEnd('\') + '\'
    $objects = @($inventory.tests | ForEach-Object { $_.command[0] } |
            Where-Object { $_ -and [IO.Path]::GetExtension($_) -eq '.exe' } |
            ForEach-Object { ConvertTo-NormalizedPath $_ } |
            Where-Object { $_.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase) } |
            Sort-Object -Unique)
    $plugin = ConvertTo-NormalizedPath (Join-Path $buildDirectory 'Burlak.dll')
    $objects += $plugin
    foreach ($object in $objects) {
        if (-not (Test-Path -LiteralPath $object -PathType Leaf)) {
            throw "Coverage object is missing: $object"
        }
    }
    Write-Output "Coverage objects: $($objects.Count)"
    $objects | ForEach-Object { Write-Output "  $_" }

    $exportsSource = ConvertTo-NormalizedPath (Join-Path $repository 'src\plugin\Exports.cpp')
    Assert-BurlakProfile -ProfileFiles $profiles -Plugin $plugin -ExportsSource $exportsSource `
        -LlvmProfdata $llvmProfdata -LlvmCov $llvmCov -WorkDirectory $buildDirectory

    $arguments = @($objects[0])
    foreach ($object in $objects | Select-Object -Skip 1) {
        $arguments += "-object=$object"
    }
    $arguments += "-instr-profile=$profileData"
    $arguments += '-ignore-filename-regex=([\\/]tests[\\/]|[\\/]third_party[\\/]|[\\/]sdk[\\/]|[\\/]build[\\/]|Program Files|Windows Kits)'

    $report = & $llvmCov report @arguments
    if ($LASTEXITCODE -ne 0) {
        throw 'llvm-cov report failed'
    }
    $report | Write-Output

    $json = & $llvmCov export -summary-only @arguments
    if ($LASTEXITCODE -ne 0) {
        throw 'llvm-cov export failed'
    }
    $coverage = $json | ConvertFrom-Json
    $totals = $coverage.data[0].totals
    $reported = @{}
    foreach ($file in $coverage.data[0].files) {
        $reported[(ConvertTo-NormalizedPath $file.filename)] = $file
    }

    $missing = @($executableSources | Where-Object { -not $reported.ContainsKey($_.FullPath) } |
            ForEach-Object { $_.RelativePath })
    if ($missing.Count -ne 0) {
        throw "Coverage completeness failed. Missing files:`n  $($missing -join "`n  ")"
    }
    $misclassified = @($declarationSources | Where-Object {
            $reported.ContainsKey($_.FullPath) -and $reported[$_.FullPath].summary.regions.count -gt 0
        } | ForEach-Object { $_.RelativePath })
    if ($misclassified.Count -ne 0) {
        throw "Declarations-only coverage entries contain regions:`n  $($misclassified -join "`n  ")"
    }
    $undercovered = @($executableSources | ForEach-Object {
            $entry = $_
            $summary = $reported[$entry.FullPath].summary
            foreach ($metric in @('regions', 'functions', 'lines', 'branches')) {
                if ($summary.$metric.count -gt 0 -and $summary.$metric.percent -ne 100) {
                    "$($entry.RelativePath) $metric $($summary.$metric.percent)%"
                }
            }
        })
    if ($undercovered.Count -ne 0) {
        throw "Per-file coverage is below 100%:`n  $($undercovered -join "`n  ")"
    }
    Write-Output "Coverage source inventory passed: $($classified.Count) files classified; $($executableSources.Count) executable files present in llvm-cov."

    $htmlDirectory = Join-Path $buildDirectory 'html'
    $expectedPrefix = (ConvertTo-NormalizedPath (Join-Path $repository 'build')).TrimEnd('\') + '\'
    if (-not (ConvertTo-NormalizedPath $htmlDirectory).StartsWith($expectedPrefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace an HTML directory outside $expectedPrefix"
    }
    if (Test-Path -LiteralPath $htmlDirectory) {
        Remove-Item -LiteralPath $htmlDirectory -Recurse -Force
    }
    Invoke-Checked $llvmCov (@('show') + $arguments + @('-format=html', "-output-dir=$htmlDirectory"))

    if ($totals.lines.percent -ne 100 -or $totals.branches.percent -ne 100) {
        throw "Coverage gate failed: lines $($totals.lines.percent)%, branches $($totals.branches.percent)%"
    }
    Write-Output 'Coverage gate passed: lines 100%, branches 100%.'
    Write-Output "HTML report: $htmlDirectory"
}
finally {
    if ($hadProfile) {
        $env:LLVM_PROFILE_FILE = $oldProfile
    }
    else {
        Remove-Item -LiteralPath 'Env:LLVM_PROFILE_FILE' -ErrorAction SilentlyContinue
    }
    Pop-Location
}
