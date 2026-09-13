[CmdletBinding()]
param(
    [string] $BuildDir = 'build/debug',
    [string[]] $ReleaseDirs = @('build/release-x64', 'build/release-x86', 'build/release-arm64'),
    [string] $LlvmDir,
    [string[]] $Tools = @('clang-format', 'clang-tidy', 'cppcheck', 'psscriptanalyzer', 'binskim'),
    [int] $Jobs = [Math]::Max(1, [Math]::Floor([Environment]::ProcessorCount / 2))
)

# Runs the five repository gates and prints every finding in compiler-style form. LLVM can be
# supplied explicitly; otherwise Visual Studio's bundled LLVM is found through vswhere or the
# standard Build Tools / Enterprise locations used locally and by windows-2022 runners.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDir = [IO.Path]::GetFullPath((Join-Path $root $BuildDir))
$ReleaseDirs = @($ReleaseDirs | ForEach-Object { $_ -split ',' } | ForEach-Object {
        [IO.Path]::GetFullPath((Join-Path $root $_.Trim()))
    })
$Tools = @($Tools | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToLowerInvariant() })
$Jobs = [Math]::Max(1, $Jobs)
$knownTools = @('clang-format', 'clang-tidy', 'cppcheck', 'psscriptanalyzer', 'binskim')
foreach ($tool in $Tools) {
    if ($knownTools -notcontains $tool) { throw "Unknown lint tool '$tool'" }
}

function Find-LlvmDirectory {
    if ($LlvmDir) {
        return [IO.Path]::GetFullPath($LlvmDir)
    }
    $candidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\x64\bin"
    )
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installations = @(& $vswhere -products '*' -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang `
                -property installationPath)
        $candidates += @($installations | Where-Object { $_ } | ForEach-Object {
                Join-Path $_ 'VC\Tools\Llvm\x64\bin'
            })
    }
    $found = $candidates | Where-Object {
        Test-Path -LiteralPath (Join-Path $_ 'clang-tidy.exe') -PathType Leaf
    } | Select-Object -First 1
    if (-not $found) { throw 'Visual Studio LLVM tools were not found; pass -LlvmDir' }
    return [IO.Path]::GetFullPath($found)
}

$LlvmDir = Find-LlvmDirectory

function Get-ToolPath {
    param([string] $Name, [switch] $UseLlvm)
    if ($UseLlvm) {
        $path = Join-Path $LlvmDir "$Name.exe"
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "$Name was not found at $path" }
        return $path
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) { throw "$Name was not found on PATH" }
    return $command.Source
}

function Invoke-NativeTool {
    param([string] $Command, [string[]] $Arguments)
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $Command @Arguments 2>&1 | ForEach-Object { "$_" })
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $output }
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
}

function Get-SourceFile {
    param([string[]] $Extensions)
    return @(Get-ChildItem -LiteralPath (Join-Path $root 'src'), (Join-Path $root 'tests') -Recurse -File |
            Where-Object { $Extensions -contains $_.Extension } | Sort-Object FullName |
            ForEach-Object { $_.FullName })
}

function Invoke-ClangFormatGate {
    $tool = Get-ToolPath clang-format -UseLlvm
    $files = @(Get-SourceFile @('.cpp', '.hpp', '.h'))
    $findings = @()
    for ($start = 0; $start -lt $files.Count; $start += 40) {
        $end = [Math]::Min($start + 39, $files.Count - 1)
        $run = Invoke-NativeTool $tool (@('--dry-run', '--Werror', '--style=file') + @($files[$start..$end]))
        $diagnostics = @($run.Output | Where-Object { $_ -match '^.+?:\d+:\d+: (error|warning): ' })
        if ($run.ExitCode -ne 0 -and $diagnostics.Count -eq 0) {
            throw "clang-format failed without a diagnostic:`n$($run.Output -join "`n")"
        }
        $findings += $diagnostics
    }
    return [pscustomobject]@{ Count = $findings.Count; Lines = $findings }
}

function Get-CompileSource {
    $database = Join-Path $BuildDir 'compile_commands.json'
    if (-not (Test-Path -LiteralPath $database -PathType Leaf)) {
        throw "$database is missing; configure and build the debug preset first"
    }
    $rootPrefix = $root.TrimEnd('\') + '\'
    $buildPrefix = $BuildDir.TrimEnd('\') + '\'
    $entries = @(Get-JsonItem (Get-Content -LiteralPath $database -Raw | ConvertFrom-Json))
    return @($entries | ForEach-Object { [IO.Path]::GetFullPath($_.file) } | Where-Object {
            $_.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase) -and
            -not $_.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetExtension($_) -eq '.cpp'
        } | Sort-Object -Unique)
}

function Invoke-ClangTidyGate {
    param([int] $ParallelJobs)
    $tool = Get-ToolPath clang-tidy -UseLlvm
    $findings = @()
    $seen = @{}
    $pool = [runspacefactory]::CreateRunspacePool(1, [Math]::Max(1, $ParallelJobs))
    $pool.Open()
    $tasks = @()
    try {
        foreach ($source in @(Get-CompileSource)) {
            $shell = [powershell]::Create()
            $shell.RunspacePool = $pool
            [void] $shell.AddScript({
                    param($Tool, $Database, $File)
                    $output = @(& $Tool -p $Database --quiet $File 2>&1 | ForEach-Object { "$_" })
                    [pscustomobject]@{ File = $File; ExitCode = $LASTEXITCODE; Output = $output }
                }).AddArgument($tool).AddArgument($BuildDir).AddArgument($source)
            $tasks += [pscustomobject]@{ Shell = $shell; Handle = $shell.BeginInvoke() }
        }
        foreach ($task in $tasks) {
            $run = $task.Shell.EndInvoke($task.Handle)[0]
            $task.Shell.Dispose()
            $diagnostics = @($run.Output | Where-Object {
                    $_ -match '^(?<file>.+?):(?<line>\d+):(?<column>\d+): (error|warning): .+ \[(?<check>[^]]+)\]$'
                })
            foreach ($line in $diagnostics) {
                $null = $line -match '^(?<file>.+?):(?<line>\d+):(?<column>\d+): .+ \[(?<check>[^]]+)\]$'
                $key = "$($Matches.file):$($Matches.line):$($Matches.column):$($Matches.check)".ToLowerInvariant()
                if (-not $seen.ContainsKey($key)) {
                    $seen[$key] = $true
                    $findings += $line
                }
            }
            if ($run.ExitCode -ne 0 -and $diagnostics.Count -eq 0) {
                throw "clang-tidy failed on $($run.File) without a diagnostic:`n$($run.Output -join "`n")"
            }
        }
    }
    finally {
        $pool.Close()
        $pool.Dispose()
    }
    return [pscustomobject]@{ Count = $findings.Count; Lines = $findings }
}

function Invoke-CppcheckGate {
    $tool = Get-ToolPath cppcheck
    $database = Join-Path $BuildDir 'compile_commands.json'
    $arguments = @(
        "--project=$database", '--file-filter=*/src/*', '--enable=warning,performance,portability',
        '--std=c++23', '--platform=win64', '--inline-suppr', '--error-exitcode=1', '--library=windows',
        "--suppressions-list=$(Join-Path $root 'cppcheck-suppressions.txt')",
        '--template={file}:{line}:{column}: {severity}: {message} [{id}]', '--quiet'
    )
    $run = Invoke-NativeTool $tool $arguments
    $findings = @($run.Output | Where-Object {
            $_ -match '^.+?:\d+:\d+: (error|warning|performance|portability|style|information): .+ \[[\w-]+\]$'
        })
    if ($run.ExitCode -ne 0 -and $findings.Count -eq 0) {
        throw "cppcheck failed without a diagnostic:`n$($run.Output -join "`n")"
    }
    return [pscustomobject]@{ Count = $findings.Count; Lines = $findings }
}

function Invoke-PSScriptAnalyzerGate {
    Import-Module PSScriptAnalyzer -ErrorAction Stop
    $settings = Join-Path $root 'PSScriptAnalyzerSettings.psd1'
    $diagnostics = @(Invoke-ScriptAnalyzer -Path (Join-Path $root 'scripts') -Recurse -Settings $settings)
    foreach ($dataFile in @(Get-ChildItem -LiteralPath $root -File -Filter '*.psd1')) {
        $diagnostics += @(Invoke-ScriptAnalyzer -Path $dataFile.FullName -Settings $settings)
    }
    $findings = @($diagnostics | ForEach-Object {
            "$($_.ScriptPath):$($_.Line):$($_.Column): $($_.Severity.ToString().ToLowerInvariant()): " +
            "$($_.Message) [$($_.RuleName)]"
        })
    return [pscustomobject]@{ Count = $findings.Count; Lines = $findings }
}

function Get-JsonItem {
    param($Value)
    return @($Value | Where-Object { $null -ne $_ })
}

function Test-HighEntropyVirtualAddress {
    param([string] $Image)
    $readObject = Get-ToolPath llvm-readobj -UseLlvm
    $run = Invoke-NativeTool $readObject @('--file-headers', $Image)
    if ($run.ExitCode -ne 0) { throw "llvm-readobj failed on $Image" }
    $is64 = [bool]($run.Output | Where-Object { $_ -match '^\s*Magic:\s*0x20B\s*$' })
    $enabled = [bool]($run.Output | Where-Object { $_ -match 'IMAGE_DLL_CHARACTERISTICS_HIGH_ENTROPY_VA' })
    if ($is64 -and -not $enabled) {
        return "$Image`:0:0: error: 64-bit image lacks /HIGHENTROPYVA [HighEntropyVA]"
    }
}

function Invoke-BinSkimGate {
    $tool = Get-ToolPath binskim
    $policy = Import-PowerShellDataFile -LiteralPath (Join-Path $root 'binskim.psd1')
    $findings = @()
    foreach ($directory in $ReleaseDirs) {
        $dll = Join-Path $directory 'Burlak.dll'
        if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { throw "BinSkim input is missing: $dll" }
        $reportDirectory = Join-Path $directory 'lint'
        New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
        $report = Join-Path $reportDirectory 'Burlak.binskim.sarif'
        $run = Invoke-NativeTool $tool @('analyze', $dll, '--output', $report, '--log', 'ForceOverwrite',
            '--level', 'Error;Warning', '--kind', 'Fail', '--ignorePdbLoadError', '--disable-telemetry')
        if (-not (Test-Path -LiteralPath $report -PathType Leaf)) {
            throw "BinSkim did not write $report`n$($run.Output -join "`n")"
        }
        $sarif = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
        foreach ($sarifRun in @(Get-JsonItem $sarif.runs)) {
            foreach ($result in @(Get-JsonItem $sarifRun.results)) {
                $level = if ($result.level) { $result.level } elseif ($result.kind -and $result.kind -ne 'fail') {
                    'none'
                } else {
                    'warning'
                }
                if ($policy.FailOnLevels -contains $level -and
                    -not $policy.AcceptedResults.ContainsKey($result.ruleId)) {
                    $message = "$(($result.message.text -split "`r?`n")[0])"
                    $findings += "$dll`:0:0: $level`: $message [$($result.ruleId)]"
                }
            }
            foreach ($invocation in @(Get-JsonItem $sarifRun.invocations)) {
                $notifications = @(Get-JsonItem $invocation.toolExecutionNotifications) +
                    @(Get-JsonItem $invocation.toolConfigurationNotifications)
                foreach ($notification in $notifications) {
                    $id = $notification.descriptor.id
                    if ($notification.level -eq 'error' -and -not $policy.AcceptedNotifications.ContainsKey($id)) {
                        $findings += "$dll`:0:0: error: $($notification.message.text) [$id]"
                    }
                }
            }
        }
        $entropyFinding = Test-HighEntropyVirtualAddress $dll
        if ($entropyFinding) { $findings += $entropyFinding }
        if ($run.ExitCode -ne 0 -and $findings.Count -eq 0) {
            throw "BinSkim exited with $($run.ExitCode) on $dll without a finding"
        }
    }
    return [pscustomobject]@{ Count = $findings.Count; Lines = $findings }
}

$steps = [ordered]@{
    'clang-format' = { Invoke-ClangFormatGate }
    'clang-tidy' = { Invoke-ClangTidyGate $Jobs }
    'cppcheck' = { Invoke-CppcheckGate }
    'psscriptanalyzer' = { Invoke-PSScriptAnalyzerGate }
    'binskim' = { Invoke-BinSkimGate }
}
$total = 0
$summary = @()
foreach ($name in $steps.Keys) {
    if ($Tools -notcontains $name) { continue }
    Write-Output "== $name"
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $result = & $steps[$name]
    $result.Lines | Write-Output
    $total += $result.Count
    $summary += "$name`: $($result.Count) finding(s) in $([Math]::Round($stopwatch.Elapsed.TotalSeconds, 1)) s"
}
Write-Output '== summary'
$summary | Write-Output
if ($total -ne 0) {
    [Console]::Error.WriteLine("lint: $total finding(s)")
    exit 1
}
Write-Output 'lint: clean'
