param([Parameter(Mandatory)][string] $PackageRoot, [Parameter(Mandatory)][string] $Mode)
$ErrorActionPreference = 'Stop'
$env:BLANK_VM_LAB = '1'
$env:QT_FATAL_WARNINGS = '1'
$status = Join-Path $PackageRoot "$Mode.status.json"
$identity = [pscustomobject]@{
    User = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    Elevated = ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    SessionId = (Get-Process -Id $PID).SessionId
}
[IO.File]::WriteAllText((Join-Path $PackageRoot "$Mode.identity.json"), ($identity | ConvertTo-Json), [Text.UTF8Encoding]::new($false))

$process = Start-Process -FilePath (Join-Path $PackageRoot 'backend_lab.exe') -WorkingDirectory $PackageRoot -WindowStyle Hidden -PassThru -ArgumentList @(
    $Mode, ('"' + (Join-Path $PackageRoot 'core/mihomo.exe') + '"'), ('"' + $status + '"')
) -RedirectStandardOutput (Join-Path $PackageRoot "$Mode.stdout.txt") -RedirectStandardError (Join-Path $PackageRoot "$Mode.stderr.txt")
$process.Id | Set-Content -LiteralPath (Join-Path $PackageRoot "$Mode.host.pid")
# Windows PowerShell can otherwise release its process handle before ExitCode is queried.
$nativeHandle = $process.Handle
if (-not $process.WaitForExit(210000)) {
    # The exact child was started by this script; its owned Job and proxy guard
    # perform abnormal-exit recovery. Never kill by a process name.
    $process.Kill()
    $process.WaitForExit()
    $exitCode = 124
} else {
    $exitCode = $process.ExitCode
    if ($null -eq $exitCode) { $exitCode = 125 }
}
$exitCode | Set-Content -LiteralPath (Join-Path $PackageRoot "$Mode.exit.txt")
$process.Dispose()
