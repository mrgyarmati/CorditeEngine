<#
.SYNOPSIS
    Windows developer entry point: configure, build, test, format, lint and run Cordite.

.DESCRIPTION
    Loads the MSVC build environment, puts the bootstrapped tools from .tools/ on PATH and
    forwards to CMake/CTest. Linux and macOS developers call the CMake presets directly
    (see AGENTS.md).

.PARAMETER Command
    configure     Generate the build tree for the selected preset.
    build         Build one target or everything (configures first when needed).
    test          Run CTest for the selected configuration.
    format        Reformat all C++ sources in place with clang-format.
    format-check  Fail when any C++ source is not clang-format clean.
    tidy          Run clang-tidy over the engine sources (uses the windows-clang preset
                  unless -Preset is given; that preset must be configured first). It
                  takes roughly half a minute per file, spread over all cores; pass
                  -Changed to lint only what uncommitted changes affect.
    verify        Run the whole pre-commit gate: format-check, MSVC builds and tests in
                  all three configurations, clang-cl build and tests, and tidy on the
                  changed files. Prints one PASS/FAIL line per step. '-Filter quick'
                  runs only the first three steps.
    run           Run a built executable (-Target) with the remaining arguments.
    clean         Delete the build tree of the selected preset.

.PARAMETER Config
    Build configuration. RelWithDebInfo keeps assertions enabled and is the default.

.PARAMETER Preset
    CMake configure preset: 'windows' (MSVC) or 'windows-clang' (clang-cl).

.EXAMPLE
    ./scripts/dev.ps1 build
    ./scripts/dev.ps1 test -Filter math
    ./scripts/dev.ps1 tidy -Filter core
    ./scripts/dev.ps1 run -Target cordite-editor -- --project projects/showcase
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('configure', 'build', 'test', 'format', 'format-check', 'tidy', 'verify', 'run', 'clean')]
    [string]$Command,

    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',

    [ValidateSet('windows', 'windows-clang')]
    [string]$Preset = 'windows',

    # build/run: target name. Empty builds everything.
    [string]$Target = '',

    # test: CTest name regex (-R). tidy: regex matched against source file paths.
    # verify: 'quick' to run only format-check and the default build and tests.
    [string]$Filter = '',

    # test: only run tests with this CTest label (-L).
    [string]$Label = '',

    # test: skip tests with this CTest label (-LE), e.g. 'gpu' on machines without Vulkan.
    [string]$ExcludeLabel = '',

    # tidy: lint only sources affected by uncommitted changes instead of everything.
    [switch]$Changed,

    # run: arguments forwarded to the executable. configure: extra CMake arguments.
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolsDir = Join-Path $repoRoot '.tools'

# clang-tidy needs compile commands written for a Clang driver.
if ($Command -eq 'tidy' -and -not $PSBoundParameters.ContainsKey('Preset')) {
    $Preset = 'windows-clang'
}
$buildDir = Join-Path $repoRoot "build\$Preset"

# Directories that hold C++ sources we format. Only $lintDirectories are linted: tests
# lean on doctest macros that clang-tidy cannot analyse usefully.
$sourceDirectories = @('engine', 'editor', 'runtime', 'tests', 'tools')
$lintDirectories = @('engine', 'editor', 'runtime', 'tools')

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe was not found. Install Visual Studio 2022 with the "Desktop development with C++" workload.'
    }

    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $path) {
        throw 'No Visual Studio installation with the MSVC x64 toolset was found.'
    }
    return $path
}

function Import-BuildEnvironment {
    foreach ($tool in @('cmake\bin', 'ninja')) {
        $toolPath = Join-Path $toolsDir $tool
        if (Test-Path $toolPath) {
            $env:PATH = "$toolPath;$env:PATH"
        }
    }

    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw 'cmake was not found. Run scripts/bootstrap.ps1 first.'
    }

    # Let executables started through this script find the bootstrapped Vulkan validation
    # layers. CTest sets the same variable for GPU tests on its own.
    $layerPath = Join-Path $toolsDir 'vulkan-sdk\Bin'
    if (-not $env:VK_ADD_LAYER_PATH -and (Test-Path (Join-Path $layerPath 'VkLayer_khronos_validation.json'))) {
        $env:VK_ADD_LAYER_PATH = $layerPath
    }

    # Already inside a Visual Studio developer prompt.
    if ($env:VSCMD_VER) {
        return
    }

    $vcvars = Join-Path (Find-VisualStudio) 'VC\Auxiliary\Build\vcvars64.bat'
    $dump = & cmd.exe /c "`"$vcvars`" > nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to load the MSVC environment from $vcvars."
    }

    foreach ($line in $dump) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
        }
    }
}

function Invoke-Checked {
    param([string]$Executable, [string[]]$ArgumentList)

    & $Executable @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Invoke-Configure {
    Invoke-Checked cmake (@('--preset', $Preset) + $Arguments)
}

# Finds an LLVM tool on PATH or in the LLVM distribution bundled with Visual Studio.
function Get-LlvmTool {
    param([string]$Name)

    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }

    $bundled = Join-Path (Find-VisualStudio) "VC\Tools\Llvm\x64\bin\$Name.exe"
    if (-not (Test-Path $bundled)) {
        throw "$Name was not found. Install the 'C++ Clang tools for Windows' Visual Studio component."
    }
    return $bundled
}

function Get-SourceFiles {
    param([string[]]$Directories)

    $files = @()
    foreach ($directory in $Directories) {
        $path = Join-Path $repoRoot $directory
        if (Test-Path $path) {
            # Filter by extension explicitly: -Include is unreliable together with -Recurse.
            $files += Get-ChildItem -LiteralPath $path -Recurse -File |
                Where-Object { $_.Extension -in '.h', '.cpp', '.inl' } |
                ForEach-Object { $_.FullName }
        }
    }
    return $files
}

function Invoke-Format {
    param([switch]$CheckOnly)

    $clangFormat = Get-LlvmTool 'clang-format'
    $files = @(Get-SourceFiles -Directories $sourceDirectories)
    if ($files.Count -eq 0) {
        Write-Host 'No source files found.'
        return
    }

    [string[]]$options = @('--style=file')
    if ($CheckOnly) {
        $options += @('--dry-run', '--Werror')
    }
    else {
        $options += '-i'
    }

    # Windows limits command lines to 32k characters, so format in batches.
    $batchSize = 100
    $failed = $false
    for ($index = 0; $index -lt $files.Count; $index += $batchSize) {
        $last = [math]::Min($index + $batchSize, $files.Count) - 1
        [string[]]$batch = $files[$index..$last]
        & $clangFormat @options @batch
        if ($LASTEXITCODE -ne 0) {
            $failed = $true
        }
    }

    if ($failed) {
        Write-Host 'Formatting check failed. Run: scripts/dev.ps1 format'
        exit 1
    }
    Write-Host ("clang-format: {0} files {1}." -f $files.Count, $(if ($CheckOnly) { 'clean' } else { 'formatted' }))
}

# Narrows a list of .cpp files to those affected by uncommitted changes: files that
# changed themselves, plus every source in a directory where a header changed. A header
# can also affect other directories; run a full 'tidy' before a large commit.
function Select-ChangedSources {
    param([string[]]$Sources)

    $status = & git -C $repoRoot status --porcelain --untracked-files=all
    if ($LASTEXITCODE -ne 0) {
        throw 'tidy -Changed needs a git repository.'
    }

    $changedSources = @{}
    $changedHeaderDirectories = @{}
    foreach ($line in $status) {
        # Porcelain format: two status characters, a space, then the path. Renames are
        # reported as "old -> new".
        $path = ($line.Substring(3) -split ' -> ')[-1].Trim('"')
        if ($path -eq '.clang-tidy') {
            return $Sources
        }
        $full = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $path))
        switch ([System.IO.Path]::GetExtension($full)) {
            '.cpp' { $changedSources[$full] = $true }
            { $_ -in '.h', '.inl' } { $changedHeaderDirectories[[System.IO.Path]::GetDirectoryName($full)] = $true }
        }
    }

    return $Sources | Where-Object {
        $changedSources.ContainsKey($_) -or $changedHeaderDirectories.ContainsKey([System.IO.Path]::GetDirectoryName($_))
    }
}

function Invoke-Tidy {
    if (-not (Test-Path (Join-Path $buildDir 'compile_commands.json'))) {
        throw "No compile_commands.json in $buildDir. Run: scripts/dev.ps1 configure -Preset $Preset"
    }

    $clangTidy = Get-LlvmTool 'clang-tidy'
    $files = @(Get-SourceFiles -Directories $lintDirectories | Where-Object { $_ -like '*.cpp' })
    if ($Changed) {
        $files = @(Select-ChangedSources -Sources $files)
    }
    if ($Filter) {
        $files = @($files | Where-Object { $_ -match $Filter })
    }
    if ($files.Count -eq 0) {
        Write-Output 'clang-tidy: no source files to lint.'
        return
    }

    $logDir = Join-Path $buildDir 'tidy'
    if (Test-Path $logDir) {
        Remove-Item -LiteralPath $logDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null

    # clang-tidy is single-threaded, so run one process per core, each on a slice of the files.
    $workerCount = [math]::Max(1, [math]::Min([int]$env:NUMBER_OF_PROCESSORS, $files.Count))
    $workers = @()
    for ($worker = 0; $worker -lt $workerCount; $worker++) {
        $slice = @(for ($index = $worker; $index -lt $files.Count; $index += $workerCount) { $files[$index] })
        $log = Join-Path $logDir "worker-$worker.log"
        $argumentList = @('-p', "`"$buildDir`"", '--quiet') + ($slice | ForEach-Object { "`"$_`"" })
        $process = Start-Process -FilePath $clangTidy -ArgumentList $argumentList -NoNewWindow -PassThru `
            -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
        # Touching the handle keeps ExitCode readable after the process has exited.
        $null = $process.Handle
        $workers += @{ Process = $process; Log = $log }
    }

    $failed = $false
    $diagnostics = @()
    foreach ($entry in $workers) {
        $entry.Process.WaitForExit()
        if ($entry.Process.ExitCode -ne 0) {
            $failed = $true
        }
        # A header is analysed once per translation unit that includes it, so the same
        # finding appears many times. Report each diagnostic line once; the full output,
        # with source excerpts and notes, stays in the worker logs.
        $diagnostics += Get-Content -LiteralPath $entry.Log -ErrorAction SilentlyContinue |
            Where-Object { $_ -match ': (warning|error): ' }
    }

    $prefix = [regex]::Escape($repoRoot + '\')
    $diagnostics | Sort-Object -Unique | ForEach-Object { ($_ -replace "^$prefix", '') -replace '/', '\' } | Write-Output

    if ($failed) {
        Write-Output "clang-tidy reported problems. Full output: $logDir"
        exit 1
    }
    Write-Output ("clang-tidy: {0} files clean." -f $files.Count)
}

# Runs this script again in a child process for one verification step, so that a failing
# step cannot terminate the whole run. Returns $true when the step succeeded.
function Invoke-VerifyStep {
    param([string]$Name, [string[]]$StepArguments, [string]$LogDir)

    $log = Join-Path $LogDir (($Name -replace '[^A-Za-z0-9]+', '-') + '.log')
    $argumentList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"") + $StepArguments
    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath 'powershell.exe' -ArgumentList $argumentList -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
    $null = $process.Handle
    $process.WaitForExit()
    $seconds = [math]::Round($stopwatch.Elapsed.TotalSeconds, 1)

    if ($process.ExitCode -eq 0) {
        Write-Output ("[PASS] {0} ({1}s)" -f $Name, $seconds)
        return $true
    }

    Write-Output ("[FAIL] {0} ({1}s) - log: {2}" -f $Name, $seconds, $log)
    $pattern = '\): (fatal )?error |: warning |error: |warning: |LNK\d+|ERROR: |\*\*\*Failed|TEST CASE:|is NOT correct|values: |threw exception|tests failed|Formatting check failed'
    Get-Content -LiteralPath $log, "$log.stderr" -ErrorAction SilentlyContinue |
        Where-Object { $_ -match $pattern } |
        Select-Object -First 40 |
        ForEach-Object { '       ' + $_.Trim() }
    return $false
}

# The complete pre-commit gate: formatting, both compilers, every configuration, all
# tests and the linter. -Filter 'quick' limits it to the default configuration.
function Invoke-Verify {
    $logDir = Join-Path $repoRoot 'build\verify'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null

    $steps = @(
        @{ Name = 'format-check'; Arguments = @('format-check') },
        @{ Name = 'build msvc RelWithDebInfo'; Arguments = @('build') },
        @{ Name = 'test msvc RelWithDebInfo'; Arguments = @('test') }
    )
    if ($Filter -ne 'quick') {
        $steps += @(
            @{ Name = 'build msvc Release'; Arguments = @('build', '-Config', 'Release') },
            @{ Name = 'test msvc Release'; Arguments = @('test', '-Config', 'Release') },
            @{ Name = 'build msvc Debug'; Arguments = @('build', '-Config', 'Debug') },
            @{ Name = 'test msvc Debug'; Arguments = @('test', '-Config', 'Debug') },
            @{ Name = 'build clang-cl RelWithDebInfo'; Arguments = @('build', '-Preset', 'windows-clang') },
            @{ Name = 'test clang-cl RelWithDebInfo'; Arguments = @('test', '-Preset', 'windows-clang') },
            @{ Name = 'tidy (changed files)'; Arguments = @('tidy', '-Changed') }
        )
    }

    $failures = 0
    foreach ($step in $steps) {
        # A build that fails makes the test step after it meaningless, but independent
        # steps still run so one pass reports everything that is wrong.
        $results = @(Invoke-VerifyStep -Name $step.Name -StepArguments $step.Arguments -LogDir $logDir)
        $results | Where-Object { $_ -is [string] } | Write-Output
        if ($results[-1] -ne $true) {
            $failures++
        }
    }

    if ($failures -gt 0) {
        Write-Output "VERIFY FAILED: $failures step(s) failed."
        exit 1
    }
    Write-Output 'VERIFY PASSED'
}

switch ($Command) {
    'format' { Invoke-Format; return }
    'format-check' { Invoke-Format -CheckOnly; return }
    'tidy' { Invoke-Tidy; return }
    'verify' { Invoke-Verify; return }
    'clean' {
        if (Test-Path $buildDir) {
            Remove-Item -LiteralPath $buildDir -Recurse -Force
        }
        return
    }
}

Import-BuildEnvironment

switch ($Command) {
    'configure' {
        Invoke-Configure
    }
    'build' {
        if (-not (Test-Path (Join-Path $buildDir 'build.ninja'))) {
            Invoke-Configure
        }
        $buildArguments = @('--build', $buildDir, '--config', $Config)
        if ($Target) {
            $buildArguments += @('--target', $Target)
        }
        Invoke-Checked cmake $buildArguments
    }
    'test' {
        $testArguments = @('--test-dir', $buildDir, '-C', $Config, '--output-on-failure', '--parallel', '16')
        if ($Filter) { $testArguments += @('-R', $Filter) }
        if ($Label) { $testArguments += @('-L', $Label) }
        if ($ExcludeLabel) { $testArguments += @('-LE', $ExcludeLabel) }
        Invoke-Checked ctest $testArguments
    }
    'run' {
        if (-not $Target) {
            throw 'run requires -Target <executable name>.'
        }
        $executable = Join-Path $buildDir "bin\$Config\$Target.exe"
        if (-not (Test-Path $executable)) {
            throw "$executable does not exist. Build it first: scripts/dev.ps1 build -Target $Target -Config $Config"
        }
        Invoke-Checked $executable $Arguments
    }
}
