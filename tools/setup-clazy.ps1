[CmdletBinding()]
param(
    [string] $Destination = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out/tools/clazy')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$projectRoot = Split-Path -Parent $PSScriptRoot
$packageLockPath = Join-Path $PSScriptRoot 'clazy-packages.lock.json'
$packageLock = Get-Content -LiteralPath $packageLockPath -Raw | ConvertFrom-Json
$cacheRoot = Join-Path $projectRoot 'out/tools/clazy-downloads/archives'
New-Item -ItemType Directory -Path $Destination, $cacheRoot -Force | Out-Null

foreach ($package in $packageLock.packages) {
    $archivePath = Join-Path $cacheRoot $package.file
    if (-not (Test-Path -LiteralPath $archivePath)) {
        Write-Host "Downloading $($package.name) $($package.version)..."
        Invoke-WebRequest -Uri "$($packageLock.repository)/$($package.file)" -OutFile $archivePath
    }
    $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    if ($actualHash -ine $package.sha256) {
        throw "SHA256 mismatch: $archivePath. The archive was not extracted."
    }
    [string[]] $selectedPaths = @(if ($package.PSObject.Properties.Name -contains 'paths') { $package.paths })
    & tar.exe -xf $archivePath -C $Destination @selectedPaths
    if ($LASTEXITCODE -ne 0) { throw "Cannot extract $($package.file)." }
}

$executable = Join-Path $Destination 'clang64/bin/clazy-standalone.exe'
$version = & $executable --version
if ($LASTEXITCODE -ne 0) { throw "Native Clazy cannot start: $executable" }
Copy-Item -LiteralPath $packageLockPath -Destination (Join-Path $Destination 'packages.lock.json')
Write-Host ($version -join "`n")
Write-Host "Windows native Clazy is ready: $executable"
