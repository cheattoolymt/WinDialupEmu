# Run elevated on Windows with RAS disconnected; supply a reachable IPv4
# endpoint and optionally an IPv6 endpoint. No COM pair or libslirp required.
param(
    [string]$Helper = '.\work\test_netguard.exe',
    [string]$IPv4 = '1.1.1.1',
    [string]$IPv6 = '',
    [int]$Port = 443
)
$ErrorActionPreference = 'Stop'
function Test-DirectTcp([string]$Address) {
    $ip = [System.Net.IPAddress]::Parse($Address)
    $client = [System.Net.Sockets.TcpClient]::new($ip.AddressFamily)
    try {
        $connect = $client.ConnectAsync($ip, $Port)
        if (-not $connect.Wait(3000)) { return $false }
        return $client.Connected
    } catch { return $false } finally { $client.Dispose() }
}
$addresses = @($IPv4)
if ($IPv6) { $addresses += $IPv6 }
foreach ($address in $addresses) {
    if (-not (Test-DirectTcp $address)) { throw "Baseline cannot reach $address" }
}
foreach ($forceKill in @($false, $true)) {
    $started = $false
    $p = [System.Diagnostics.Process]::new()
    $p.StartInfo.FileName = (Resolve-Path $Helper).Path
    $p.StartInfo.Arguments = "$IPv4 $Port"
    $p.StartInfo.UseShellExecute = $false
    $p.StartInfo.RedirectStandardInput = $true
    $p.StartInfo.RedirectStandardOutput = $true
    try {
        $started = $p.Start()
        $ready = $p.StandardOutput.ReadLineAsync()
        if (-not $ready.Wait(10000) -or $ready.Result -ne 'READY') {
            throw 'Guard/uplink failed; check elevation, BFE, and endpoint reachability'
        }
        foreach ($address in $addresses) {
            if (Test-DirectTcp $address) { throw "Direct LAN/Wi-Fi bypass: $address" }
        }
        if ($forceKill) { $p.Kill() } else { $p.StandardInput.WriteLine('exit') }
        if (-not $p.WaitForExit(10000)) { throw 'Guard did not exit' }
        foreach ($address in $addresses) {
            $restored = $false
            for ($i = 0; $i -lt 5; $i++) {
                if (Test-DirectTcp $address) { $restored = $true; break }
                Start-Sleep -Milliseconds 200
            }
            if (-not $restored) { throw "LAN/Wi-Fi not restored: $address" }
        }
        Write-Host "PASS isolation/restoration (forced exit: $forceKill)"
    } finally {
        if ($started -and -not $p.HasExited) { $p.Kill(); $p.WaitForExit() }
        $p.Dispose()
    }
}
