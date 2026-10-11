<#
.SYNOPSIS
    Downloads the pinned Windows build tools into .tools/.

.DESCRIPTION
    Cordite needs CMake and Ninja to build. This script fetches pinned, checksum-verified
    portable builds into the repository-local .tools/ directory so nothing is installed
    system-wide. Visual Studio 2022 with the "Desktop development with C++" workload must
    already be installed.

    Linux and macOS developers should install cmake and ninja from their package manager
    instead (see AGENTS.md).

.PARAMETER WithVulkanSdk
    Also installs the LunarG Vulkan SDK in copy-only mode (no registry or environment
    changes). The SDK provides the Khronos validation layers, which the test suite uses
    when present.

.PARAMETER Force
    Re-download tools even when the pinned version is already present.

.EXAMPLE
    ./scripts/bootstrap.ps1 -WithVulkanSdk
#>
[CmdletBinding()]
param(
    [switch]$WithVulkanSdk,
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$repoRoot = Split-Path -Parent $PSScriptRoot
$toolsDir = Join-Path $repoRoot '.tools'
$downloadDir = Join-Path $toolsDir 'downloads'

# Pinned tool versions. Update the version, URL and SHA-256 together.
$archiveTools = @(
    @{
        Name            = 'cmake'
        Version         = '4.4.4'
        Url             = 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip'
        Sha256          = 'bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536'
        StripComponents = 1
    },
    @{
        Name            = 'ninja'
        Version         = '1.13.2'
        Url             = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'
        Sha256          = '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'
        StripComponents = 0
    }
)

$vulkanSdk = @{
    Name    = 'vulkan-sdk'
    Version = '1.4.363.0'
    Url     = 'https://sdk.lunarg.com/sdk/download/1.4.363.0/windows/vulkansdk-windows-X64-1.4.363.0.exe'
    Sha256  = '94a82d378f7a5e3e54c9db7d2fb7016af136e14ac0a18dbf0f2f67a36352d141'
}

function Get-VerifiedDownload {
    param([string]$Url, [string]$Sha256, [string]$Destination)

    if (-not (Test-Path $Destination)) {
        Write-Host "  downloading $Url"
        Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
    }

    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Destination).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256.ToLowerInvariant()) {
        Remove-Item -LiteralPath $Destination -Force
        throw "Checksum mismatch for $Url. Expected $Sha256 but got $actual. The download was deleted."
    }
}

function Test-ToolInstalled {
    param([string]$InstallDir, [string]$Version)

    $stamp = Join-Path $InstallDir '.version'
    return (Test-Path $stamp) -and ((Get-Content -LiteralPath $stamp -Raw).Trim() -eq $Version)
}

function Set-ToolInstalled {
    param([string]$InstallDir, [string]$Version)

    Set-Content -LiteralPath (Join-Path $InstallDir '.version') -Value $Version -Encoding ascii
}

function Install-ArchiveTool {
    param([hashtable]$Tool)

    $installDir = Join-Path $toolsDir $Tool.Name
    if (-not $Force -and (Test-ToolInstalled -InstallDir $installDir -Version $Tool.Version)) {
        Write-Host "$($Tool.Name) $($Tool.Version) is already installed."
        return
    }

    Write-Host "Installing $($Tool.Name) $($Tool.Version)..."
    $archive = Join-Path $downloadDir ("{0}-{1}.zip" -f $Tool.Name, $Tool.Version)
    Get-VerifiedDownload -Url $Tool.Url -Sha256 $Tool.Sha256 -Destination $archive

    if (Test-Path $installDir) {
        Remove-Item -LiteralPath $installDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $installDir | Out-Null

    # bsdtar ships with Windows 10+ and extracts large zip files far faster than Expand-Archive.
    & tar.exe -xf $archive -C $installDir --strip-components $Tool.StripComponents
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to extract $archive (tar exit code $LASTEXITCODE)."
    }

    Set-ToolInstalled -InstallDir $installDir -Version $Tool.Version
}

function Install-VulkanSdk {
    $installDir = Join-Path $toolsDir 'vulkan-sdk'
    if (-not $Force -and (Test-ToolInstalled -InstallDir $installDir -Version $vulkanSdk.Version)) {
        Write-Host "Vulkan SDK $($vulkanSdk.Version) is already installed."
        return
    }

    Write-Host "Installing Vulkan SDK $($vulkanSdk.Version) (copy-only, no system changes)..."
    $installer = Join-Path $downloadDir ("vulkan-sdk-{0}.exe" -f $vulkanSdk.Version)
    Get-VerifiedDownload -Url $vulkanSdk.Url -Sha256 $vulkanSdk.Sha256 -Destination $installer

    if (Test-Path $installDir) {
        Remove-Item -LiteralPath $installDir -Recurse -Force
    }

    # copy_only=1 extracts the SDK without touching the registry, PATH or Start menu, so it
    # needs no elevation. RunAsInvoker stops the installer from requesting it regardless.
    $env:__COMPAT_LAYER = 'RunAsInvoker'
    try {
        $arguments = @('--root', $installDir, '--accept-licenses', '--default-answer', '--confirm-command', 'install', 'copy_only=1')
        $process = Start-Process -FilePath $installer -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
        if ($process.ExitCode -ne 0) {
            throw "The Vulkan SDK installer exited with code $($process.ExitCode)."
        }
    }
    finally {
        Remove-Item Env:__COMPAT_LAYER -ErrorAction SilentlyContinue
    }

    Set-ToolInstalled -InstallDir $installDir -Version $vulkanSdk.Version
}

New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null

foreach ($tool in $archiveTools) {
    Install-ArchiveTool -Tool $tool
}

if ($WithVulkanSdk) {
    Install-VulkanSdk
}

Write-Host ''
Write-Host 'Bootstrap complete. Use scripts/dev.ps1 to configure, build and test.'
