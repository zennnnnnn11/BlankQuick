[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $PackageRoot,
    [ValidateSet('all', 'profile-activation', 'domain-configuration', 'application-pipeline')]
    [string] $Mode = 'all'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$env:QT_FATAL_WARNINGS = '1'
$packageDirectory = [IO.Path]::GetFullPath($PackageRoot)
$programs = [ordered]@{
    'profile-activation' = 'profile_activation_test.exe'
    'domain-configuration' = 'domain_configuration_test.exe'
    'application-pipeline' = 'application_pipeline_test.exe'
}
$selected = @($programs.Keys | Where-Object { $Mode -eq 'all' -or $_ -eq $Mode })
foreach ($name in $selected) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageDirectory $programs[$name]) -PathType Leaf)) {
        throw "Missing test executable: $($programs[$name]). Build test_binaries and package with -IncludeTests."
    }
}
$results = @(
    foreach ($name in $selected) {
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $process = Start-Process -FilePath (Join-Path $packageDirectory $programs[$name]) `
            -WorkingDirectory $packageDirectory -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $packageDirectory "$name.stdout.txt") `
            -RedirectStandardError (Join-Path $packageDirectory "$name.stderr.txt")
        try {
            # Keep the native handle alive until ExitCode has been captured.
            $processHandle = $process.Handle
            if (-not $process.WaitForExit(15000)) {
                $process.Kill()
                $process.WaitForExit()
                $code = 124
            } else {
                $code = $process.ExitCode
                if ($null -eq $code) { $code = 125 }
            }
            [ordered]@{ Test = $name; ExitCode = $code; ElapsedMilliseconds = $timer.ElapsedMilliseconds }
        } finally {
            $process.Dispose()
        }
    }
)
$identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
$report = [ordered]@{ User = $identity; Tests = $results }
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $packageDirectory "$Mode.results.json") -Encoding UTF8
$report | ConvertTo-Json -Depth 5
if (@($results | Where-Object { $_.ExitCode -ne 0 }).Count -gt 0) { exit 1 }
