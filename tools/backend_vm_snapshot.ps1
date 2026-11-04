[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'probe_user_proxy.ps1')
$serviceRoot = 'C:\Program Files\BlankQuickService'
$service = Get-CimInstance Win32_Service -Filter "Name='BlankQuickService'"
$files = @(if (Test-Path -LiteralPath $serviceRoot) {
    Get-ChildItem -LiteralPath $serviceRoot -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ Path = $_.FullName.Substring($serviceRoot.Length); Hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    }
})
[ordered]@{
    Service = $(if ($service) { [ordered]@{ State = $service.State; StartMode = $service.StartMode; Account = $service.StartName; Path = $service.PathName } })
    ServiceFiles = $files
    Proxy = Get-UserProxySnapshot
    Adapters = @(Get-NetAdapter | Sort-Object InterfaceGuid | ForEach-Object { [ordered]@{ Guid = $_.InterfaceGuid; Name = $_.Name; Status = [string]$_.Status } })
    Dns = @(Get-DnsClientServerAddress | Sort-Object InterfaceIndex,AddressFamily | ForEach-Object { [ordered]@{ Interface = $_.InterfaceIndex; Family = [int]$_.AddressFamily; Servers = @($_.ServerAddresses) } })
    Routes = @(Get-NetRoute | Sort-Object InterfaceIndex,DestinationPrefix,NextHop | ForEach-Object { [ordered]@{ Interface = $_.InterfaceIndex; Prefix = $_.DestinationPrefix; NextHop = $_.NextHop; Metric = $_.RouteMetric; Protocol = [string]$_.Protocol } })
    TunJournal = Test-Path -LiteralPath 'C:\ProgramData\BlankQuickService\tun.json'
    Tasks = @(Get-ScheduledTask | Where-Object TaskName -Like 'CodexBackend*' | Sort-Object TaskName | ForEach-Object { $_.TaskName })
    Processes = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -in @('mihomo.exe','backend_lab.exe','system_proxy_guard.exe','kernel_fixture.exe','proxy_guard_fixture.exe') } | Sort-Object ExecutablePath,SessionId | ForEach-Object { [ordered]@{ Path = $_.ExecutablePath; Session = $_.SessionId } })
} | ConvertTo-Json -Depth 8
