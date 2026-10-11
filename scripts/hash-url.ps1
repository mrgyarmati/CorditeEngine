<#
.SYNOPSIS
    Prints the SHA-256 of one or more downloads, for pinning dependencies.

.DESCRIPTION
    Every third-party archive in cmake/CorditeDependencies.cmake is pinned by URL and
    SHA-256. Run this when adding or upgrading a dependency and paste the hash next to
    the URL.

.EXAMPLE
    ./scripts/hash-url.ps1 https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Url
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

foreach ($item in $Url) {
    $temp = [System.IO.Path]::GetTempFileName()
    try {
        Invoke-WebRequest -Uri $item -OutFile $temp -UseBasicParsing
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $temp).Hash.ToLowerInvariant()
        $sizeMb = [math]::Round((Get-Item -LiteralPath $temp).Length / 1MB, 2)
        Write-Output ("{0}  {1}  ({2} MB)" -f $hash, $item, $sizeMb)
    }
    finally {
        Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue
    }
}
