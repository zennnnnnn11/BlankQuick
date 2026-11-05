[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $Mihomo,
    [Parameter(Mandatory)][string] $Wintun,
    [Parameter(Mandatory)][string] $WintunLicense,
    [Parameter(Mandatory)][string] $MihomoLicense,
    [Parameter(Mandatory)][string] $Output,
    [string] $QtRoot = 'C:/Qt/6.12.0/msvc2022_64',
    [switch] $IncludeTests,
    [string] $BuildDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = if ($BuildDirectory) { [IO.Path]::GetFullPath($BuildDirectory, $projectRoot) } else { Join-Path $projectRoot 'out/build/release' }
$build = Join-Path $buildRoot 'backend-bin'
$lock = Get-Content -LiteralPath (Join-Path $projectRoot 'src/services/kernel/dependencies.lock.json') -Raw | ConvertFrom-Json
foreach ($dependency in @(@{ Path = $Mihomo; Hash = $lock.mihomo.sha256 }, @{ Path = $Wintun; Hash = $lock.wintun.sha256 })) {
    if ((Get-FileHash -LiteralPath $dependency.Path -Algorithm SHA256).Hash -ine $dependency.Hash) {
        throw "Pinned dependency hash mismatch: $($dependency.Path)"
    }
}
if ((Get-AuthenticodeSignature -LiteralPath $Wintun).Status -ne 'Valid') { throw 'Wintun signature is not valid.' }
$destination = [IO.Path]::GetFullPath($Output)
if ((Test-Path -LiteralPath $destination) -and @(Get-ChildItem -LiteralPath $destination -Force).Count -ne 0) {
    throw 'Use an empty output directory to avoid carrying stale deployment files.'
}
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$service = Join-Path $destination 'service'
foreach ($directory in @($destination, $service)) {
    New-Item -ItemType Directory -Path (Join-Path $directory 'core'), (Join-Path $directory 'tls') -Force | Out-Null
    Copy-Item -LiteralPath $Mihomo -Destination (Join-Path $directory 'core/mihomo.exe')
    Copy-Item -LiteralPath $Wintun -Destination (Join-Path $directory 'core/wintun.dll')
    Copy-Item -LiteralPath $WintunLicense -Destination (Join-Path $directory 'WINTUN-LICENSE.txt')
    Copy-Item -LiteralPath $MihomoLicense -Destination (Join-Path $directory 'MIHOMO-LICENSE.txt')
    foreach ($module in @('Core', 'Network', 'Concurrent', 'WebSockets')) {
        Copy-Item -LiteralPath (Join-Path $QtRoot "bin/Qt6$module.dll") -Destination $directory
    }
    Copy-Item -LiteralPath (Join-Path $QtRoot 'plugins/tls/qschannelbackend.dll') -Destination (Join-Path $directory 'tls')
    Copy-Item -LiteralPath (Join-Path $build 'kernel_signal.exe') -Destination $directory
}
Copy-Item -LiteralPath (Join-Path $QtRoot 'bin/Qt6Qml.dll') -Destination $destination
Copy-Item -LiteralPath (Join-Path $build 'system_proxy_guard.exe') -Destination $destination
Copy-Item -LiteralPath (Join-Path $build 'blank_quick_service.exe') -Destination $service
if ($IncludeTests) {
    foreach ($test in @('profile_activation_test', 'domain_configuration_test', 'application_pipeline_test', 'native_kernel_lifecycle_test')) {
        Copy-Item -LiteralPath (Join-Path $buildRoot ($test + '.exe')) -Destination $destination
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'backend_vm_guest.ps1') -Destination $destination
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$redist = Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Redist/MSVC') -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'x64/Microsoft.VC143.CRT') } | Sort-Object Name -Descending | Select-Object -First 1
$crt = @(Get-ChildItem -Path (Join-Path $redist.FullName 'x64/Microsoft.VC143.CRT/*.dll') -File)
if ($crt.Count -eq 0) { throw 'VC runtime deployment files were not found.' }
foreach ($directory in @($destination, $service)) { $crt | Copy-Item -Destination $directory }
Copy-Item -LiteralPath (Join-Path $projectRoot 'src/services/kernel/NOTICE.txt') -Destination $destination
$manifest = @(Get-ChildItem -LiteralPath $destination -Recurse -File | ForEach-Object {
    @{ Path = [IO.Path]::GetRelativePath($destination, $_.FullName).Replace('\', '/');
       Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
ConvertTo-Json -InputObject $manifest -Depth 4 | Set-Content -LiteralPath (Join-Path $destination 'manifest.json') -Encoding utf8NoBOM
Write-Host "Backend package created: $destination"
