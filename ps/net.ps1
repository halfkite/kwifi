<#
  RedianWiFi - network control plane for Windows 10/11.
  ASCII only on purpose: this file is read by Windows PowerShell 5.1, which would
  otherwise mis-decode UTF-8 without a BOM. Localized tokens are built at runtime.
  Every action prints exactly one JSON object on stdout.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet('status','doctor','start','stop','limit','block','unblock','stealth','stealth-reset','ttl','name')]
  [string]$Action,

  [string]$Ssid,
  [string]$Password,
  [ValidateSet('auto','2.4','5')][string]$Band = 'auto',

  [string]$Ip,
  [int]$UpKbps = -1,
  [int]$DownKbps = -1,

  [ValidateSet('on','off','keep')][string]$Ipv6 = 'keep',
  [ValidateSet('on','off','keep')][string]$Multicast = 'keep',
  [ValidateSet('on','off','keep')][string]$Ttl = 'keep',

  [ValidateSet('quick','full')][string]$Detail = 'quick'
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

$script:Warnings = New-Object System.Collections.ArrayList
function Add-Warning([string]$m) { [void]$script:Warnings.Add($m) }

# localized tokens (Chinese) built without non-ASCII source bytes
$T_STARTED = [string]([char]0x5DF2 + [char]0x542F + [char]0x52A8)   # started
$T_NOTSUP  = [string]([char]0x4E0D + [char]0x652F + [char]0x6301)   # not supported

function Test-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-Nics {
  $list = @()
  foreach ($nic in [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces()) {
    if ($nic.NetworkInterfaceType -eq 'Loopback') { continue }
    $props = $null
    try { $props = $nic.GetIPProperties() } catch { continue }
    $ipv4 = @($props.UnicastAddresses | Where-Object { $_.Address.AddressFamily -eq 'InterNetwork' } | ForEach-Object { $_.Address.ToString() })
    $gw = @($props.GatewayAddresses | Where-Object { $_.Address.AddressFamily -eq 'InterNetwork' } | ForEach-Object { $_.Address.ToString() })
    $stats = $null
    try { $stats = $nic.GetIPv4Statistics() } catch { }
    $list += [pscustomobject]@{
      id          = $nic.Id
      name        = $nic.Name
      description = $nic.Description
      status      = $nic.OperationalStatus.ToString()
      speedMbps   = if ($nic.Speed -gt 0) { [int]($nic.Speed / 1MB) } else { 0 }
      mac         = ($nic.GetPhysicalAddress().ToString() -replace '(..)', '$1:').TrimEnd(':')
      ipv4        = $ipv4
      gateway     = $gw
      bytesIn     = if ($stats) { [double]$stats.BytesReceived } else { 0 }
      bytesOut    = if ($stats) { [double]$stats.BytesSent } else { 0 }
    }
  }
  ,$list
}

$script:IcsSubnet = '192.168.137'

function Get-HotspotNic($nics) {
  $prefix = $script:IcsSubnet
  $nics | Where-Object {
    $_.status -eq 'Up' -and ($_.ipv4 | Where-Object { $_ -like "$prefix.*" })
  } | Select-Object -First 1
}

function Get-UplinkNic($nics, $hotspot) {
  $nics | Where-Object {
    $_.status -eq 'Up' -and $_.gateway.Count -gt 0 -and $_.ipv4.Count -gt 0 -and (-not $hotspot -or $_.id -ne $hotspot.id)
  } | Select-Object -First 1
}

# ---------------- WinRT tethering ----------------

function Await($WinRtTask, $ResultType) {
  if (-not ('System.WindowsRuntimeSystemExtensions' -as [type])) {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime -ErrorAction Stop
  }
  $asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
      $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
      $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    })[0]
  $netTask = $asTaskGeneric.MakeGenericMethod($ResultType).Invoke($null, @($WinRtTask))
  $netTask.Wait(-1) | Out-Null
  $netTask.Result
}

function Test-WinRtTethering {
  try {
    $null = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]
    $true
  } catch { $false }
}

function Get-TetheringManager {
  if (-not (Test-WinRtTethering)) { return $null }
  # the WinRT profile query throws "Access is denied" without elevation; skip it
  if (-not (Test-Admin)) { return $null }
  $profile = $null
  try { $profile = [Windows.Networking.Connectivity.NetworkInformation]::GetInternetConnectionProfile() } catch {
    Add-Warning "internet profile unavailable: $($_.Exception.Message)"
    return $null
  }
  if (-not $profile) {
    try {
      $profile = [Windows.Networking.Connectivity.NetworkInformation]::GetConnectionProfiles() |
        Where-Object { $_.GetNetworkConnectivityLevel() -ne 'None' } | Select-Object -First 1
    } catch { }
  }
  if (-not $profile) { return $null }
  try {
    [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]::CreateFromConnectionProfile($profile)
  } catch {
    Add-Warning "tethering manager unavailable: $($_.Exception.Message)"
    $null
  }
}

function Get-TetheringState {
  $mgr = Get-TetheringManager
  if (-not $mgr) { return $null }
  try { $mgr.TetheringOperationalState.ToString() } catch { $null }
}

function Get-HostedNetworkState {
  $raw = & netsh.exe wlan show hostednetwork 2>&1 | Out-String
  if ($raw -match $T_NOTSUP) { return $null }
  $status = $null; $ssid = $null; $clients = 0
  $lines = $raw -split "`r?`n"
  foreach ($line in $lines) {
    if ($line -match '^\s*SSID[^:]*:\s*(.+)$') { $ssid = $Matches[1].Trim().Trim('"') }
    if ($line -match '^\s*[^:]*:\s*(\d+)\s*$' -and $line -match 'client') { $clients = [int]$Matches[1] }
  }
  $idx = -1
  for ($i = 0; $i -lt $lines.Length; $i++) { if ($lines[$i] -match 'Status') { $idx = $i } }
  if ($idx -ge 0 -and ($idx + 1) -lt $lines.Length) {
    $candidate = $lines[$idx + 1]
    if ($candidate -match ':\s*(.+)$') { $status = $Matches[1].Trim() }
  }
  if (-not $status) {
    for ($i = $lines.Length - 1; $i -ge 0; $i--) {
      if ($lines[$i] -match ':\s*(.+)$') { $status = $Matches[1].Trim(); break }
    }
  }
  [pscustomobject]@{ status = $status; ssid = $ssid; clients = $clients }
}

# ---------------- ICS ----------------

function Set-IcsSharing([bool]$enable, $hotspotName, $uplinkName) {
  $hnet = New-Object -ComObject HNetCfg.HNetShare
  $map = @{}
  foreach ($conn in $hnet.EnumEveryConnection) {
    $props = $hnet.NetConnectionProps($conn)
    $map[$props.Name] = $conn
  }
  if (-not $map.ContainsKey($uplinkName)) { throw "uplink connection '$uplinkName' not found" }
  if (-not $map.ContainsKey($hotspotName)) { throw "hotspot connection '$hotspotName' not found" }

  $pub = $hnet.INetSharingConfigurationForINetConnection($map[$uplinkName])
  $priv = $hnet.INetSharingConfigurationForINetConnection($map[$hotspotName])

  if ($enable) {
    if (-not $pub.SharingEnabled) { $pub.EnableSharing(0) }
    if (-not $priv.SharingEnabled) { $priv.EnableSharing(1) }
  } else {
    if ($pub.SharingEnabled) { $pub.DisableSharing() }
    if ($priv.SharingEnabled) { $priv.DisableSharing() }
  }
}

# ---------------- clients / limits ----------------

function Get-Clients {
  $prefix = $script:IcsSubnet
  $entries = @()
  $raw = & arp.exe -a 2>&1 | Out-String
  foreach ($line in ($raw -split "`r?`n")) {
    if ($line -match '^\s*(\d{1,3}(?:\.\d{1,3}){3})\s+([0-9a-fA-F]{2}(?:-[0-9a-fA-F]{2}){5})\s+(\S+)') {
      $ip = $Matches[1]
      $mac = ($Matches[2] -replace '-', ':').ToLower()
      if ($ip -notlike "$prefix.*") { continue }
      if ($ip -like "$prefix.1" -or $ip -like "$prefix.255") { continue }
      if ($mac -eq 'ff:ff:ff:ff:ff:ff') { continue }
      $entries += [pscustomobject]@{ ip = $ip; mac = $mac }
    }
  }
  ,@($entries | Sort-Object { [version]$_.ip })
}

function Get-BlockedIps {
  $out = @()
  try {
    foreach ($r in (Get-NetFirewallRule -DisplayName 'RedianWiFi-Block-*' -ErrorAction Stop)) {
      $out += ($r.DisplayName -replace '^RedianWiFi-Block-', '' -replace '-IN$', '')
    }
  } catch { }
  ,@($out | Sort-Object -Unique)
}

function Get-QosLimits {
  $out = @{}
  try {
    foreach ($p in (Get-NetQosPolicy -ErrorAction Stop | Where-Object { $_.Name -like 'RW-*' })) {
      $dir = 'down'
      if ($p.Name -like 'RW-UP-*') { $dir = 'up' }
      $target = ($p.Name -replace '^RW-(UP-|DOWN-)?', '')
      if (-not $out.ContainsKey($target)) { $out[$target] = @{ up = 0; down = 0 } }
      $out[$target][$dir] = [int]([double]$p.ThrottleRate / 1024)
    }
  } catch { }
  $out
}

# ---------------- stealth ----------------

function Get-StealthState($hotspot) {
  $state = [ordered]@{ ipv6 = $null; multicast = $null; ttl = 'unsupported' }
  if (-not $hotspot) { return $state }
  try {
    $binding = Get-NetAdapterBinding -Name $hotspot.name -ComponentID 'ms_tcpip6' -ErrorAction Stop
    $state.ipv6 = -not $binding.Enabled
  } catch { $state.ipv6 = $null }
  try {
    $rules = @(Get-NetFirewallRule -DisplayName 'RedianWiFi-Mcast-*' -ErrorAction Stop)
    $state.multicast = ($rules.Count -gt 0)
  } catch { $state.multicast = $null }

  $state.ttl = Get-TtlState
  $state
}

# ---------------- ttl helper ----------------

function Get-TtlPaths {
  $root = Split-Path $PSScriptRoot -Parent
  [pscustomobject]@{
    exe   = Join-Path $root 'native\win\ttlfix.exe'
    conf  = Join-Path $root 'data\ttlfix.conf'
    stats = Join-Path $root 'data\ttlfix-stats.json'
    pidf  = Join-Path $root 'data\ttlfix.pid'
  }
}

function Get-TtlProcess {
  $p = Get-TtlPaths
  if (-not (Test-Path $p.pidf)) { return $null }
  $procId = 0
  try { $procId = [int]((Get-Content $p.pidf -Raw).Trim()) } catch { return $null }
  if ($procId -le 0) { return $null }
  $proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
  if ($proc -and $proc.ProcessName -eq 'ttlfix') { return $proc }
  return $null
}

function Get-TtlState {
  $p = Get-TtlPaths
  if (Get-TtlProcess) { return 'active' }
  if (Test-Path $p.exe) { return 'ready' }
  return 'missing'
}

function Invoke-Ttl {
  if (-not (Test-Admin)) { throw 'administrator rights required' }
  $p = Get-TtlPaths
  $proc = Get-TtlProcess
  $want = ($Ttl -eq 'on')

  if (-not $want) {
    if ($proc) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item $p.pidf -ErrorAction SilentlyContinue
    return [ordered]@{ ok = $true; action = 'ttl'; data = [ordered]@{ running = $false }; warnings = @($script:Warnings); error = $null }
  }

  if (-not (Test-Path $p.exe)) { throw 'native\win\ttlfix.exe is missing' }
  if ($proc) {
    return [ordered]@{ ok = $true; action = 'ttl'; data = [ordered]@{ running = $true; pid = $proc.Id }; warnings = @($script:Warnings); error = $null }
  }

  New-Item -ItemType Directory -Force -Path (Split-Path $p.conf -Parent) | Out-Null
  if (-not (Test-Path $p.conf)) { Set-Content -Path $p.conf -Value "enabled=1`nttl=128" -Encoding ASCII }
  $np = Start-Process -FilePath $p.exe -ArgumentList @('--config', $p.conf, '--stats', $p.stats) -WindowStyle Hidden -PassThru
  Set-Content -Path $p.pidf -Value $np.Id -Encoding ASCII
  Start-Sleep -Milliseconds 1500
  $np.Refresh()
  if ($np.HasExited) {
    Remove-Item $p.pidf -ErrorAction SilentlyContinue
    throw "ttlfix exited with code $($np.ExitCode) - driver install or elevation failed"
  }
  [ordered]@{ ok = $true; action = 'ttl'; data = [ordered]@{ running = $true; pid = $np.Id }; warnings = @($script:Warnings); error = $null }
}

# ---------------- device naming ----------------

function Invoke-Name {
  if ([string]::IsNullOrWhiteSpace($Ip)) { throw 'IP is required' }
  $name = $null

  # NetBIOS (Windows hosts) is fast and usually carries the computer name
  try {
    $raw = & nbtstat.exe -A $Ip 2>&1 | Out-String
    $m = [regex]::Match($raw, '(?m)^\s*(\S+)\s+<00>\s+UNIQUE')
    if ($m.Success) { $name = $m.Groups[1].Value }
  } catch { }

  if (-not $name) {
    try {
      $entry = [System.Net.Dns]::GetHostEntry($Ip)
      if ($entry -and $entry.HostName) { $name = $entry.HostName.Split('.')[0] }
    } catch { }
  }

  [ordered]@{ ok = $true; action = 'name'; data = [ordered]@{ ip = $Ip; name = $name }; warnings = @($script:Warnings); error = $null }
}

# ---------------- actions ----------------

function Invoke-Status {
  param([string]$Detail = 'quick')

  $nics = Get-Nics
  $hotspot = Get-HotspotNic $nics
  $uplink = Get-UplinkNic $nics $hotspot
  $tetherState = Get-TetheringState
  $hosted = $null
  if (-not $tetherState) { $hosted = Get-HostedNetworkState }

  $method = $null; $active = $false; $ssid = $null; $stateText = 'unknown'
  if ($tetherState) {
    $method = 'winrt'
    $stateText = $tetherState
    $active = ($tetherState -eq 'On')
    try {
      $mgr = Get-TetheringManager
      if ($mgr) { $ssid = $mgr.GetCurrentAccessPointConfiguration().Ssid }
    } catch { }
  } elseif ($null -ne $hosted) {
    $method = 'hosted'
    $stateText = $hosted.status
    $active = ($hosted.status -match 'Started') -or ($hosted.status -eq $T_STARTED)
    $ssid = $hosted.ssid
  }

  $clients = @()
  if ($hotspot) { $clients = Get-Clients }

  # policies and per-adapter stealth checks cost hundreds of ms, so they are
  # only collected on an explicit full refresh
  $limits = $null; $blocked = $null; $stealth = $null
  if ($Detail -eq 'full') {
    $limits = Get-QosLimits
    $blocked = Get-BlockedIps
    $stealth = Get-StealthState $hotspot
  }

  [ordered]@{
    ok      = $true
    action  = 'status'
    data    = [ordered]@{
      admin      = (Test-Admin)
      method     = $method
      active     = [bool]$active
      state      = $stateText
      ssid       = $ssid
      clients    = @($clients)
      limits     = $limits
      blocked    = $blocked
      hotspotNic = $hotspot
      uplinkNic  = $uplink
      stealth    = $stealth
      ttlState   = (Get-TtlState)
    }
    warnings = @($script:Warnings)
    error   = $null
  }
}

function Invoke-Start {
  if (-not (Test-Admin)) { throw 'administrator rights required' }
  if ([string]::IsNullOrWhiteSpace($Ssid)) { throw 'SSID is required' }
  if ($Password.Length -lt 8 -or $Password.Length -gt 63) { throw 'password must be 8-63 characters' }

  $mgr = Get-TetheringManager
  if ($mgr) {
    try {
      $cfg = $mgr.GetCurrentAccessPointConfiguration()
      $configured = ($cfg.Ssid -eq $Ssid) -and ($cfg.Passphrase -eq $Password) -and ($Band -eq 'auto')
      $alreadyOn = ($mgr.TetheringOperationalState.ToString() -eq 'On')

      # reconfiguring the access point restarts Wi-Fi Direct and costs seconds,
      # so only do it when the requested SSID/password actually differ
      if (-not $configured) {
        $cfg.Ssid = $Ssid
        $cfg.Passphrase = $Password
        if ($Band -ne 'auto') {
          try { $cfg.Band = $(if ($Band -eq '5') { 'Five' } else { 'TwoPointFour' }) } catch { Add-Warning 'band selection unsupported on this adapter' }
        }
        $null = Await ($mgr.ConfigureAccessPointAsync($cfg)) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringAccessPointConfiguration])
      } elseif ($alreadyOn) {
        return [ordered]@{ ok = $true; action = 'start'; data = [ordered]@{ method = 'winrt'; ssid = $Ssid; reused = $true }; warnings = @($script:Warnings); error = $null }
      }

      $r = Await ($mgr.StartTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
      if ($r.Status -ne 'Success') { throw "tethering start failed: $($r.Status) $($r.AdditionalErrorMessage)" }
      return [ordered]@{ ok = $true; action = 'start'; data = [ordered]@{ method = 'winrt'; ssid = $Ssid }; warnings = @($script:Warnings); error = $null }
    } catch {
      Add-Warning "WinRT start failed, falling back to hosted network: $($_.Exception.Message)"
    }
  }

  & netsh.exe wlan set hostednetwork mode=allow "ssid=$Ssid" "key=$Password" keyUsage=persistent 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'failed to configure hosted network (adapter may not support it)' }
  & netsh.exe wlan start hostednetwork 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'failed to start hosted network (driver may not support it)' }

  Start-Sleep -Milliseconds 1500
  $nics = Get-Nics
  $hotspot = Get-HotspotNic $nics
  $uplink = Get-UplinkNic $nics $hotspot
  if ($hotspot -and $uplink) {
    try { Set-IcsSharing $true $hotspot.name $uplink.name } catch { Add-Warning "ICS enable failed: $($_.Exception.Message)" }
  } else {
    Add-Warning 'hotspot or uplink adapter not found; internet sharing was not enabled'
  }
  [ordered]@{ ok = $true; action = 'start'; data = [ordered]@{ method = 'hosted'; ssid = $Ssid }; warnings = @($script:Warnings); error = $null }
}

function Invoke-Stop {
  if (-not (Test-Admin)) { throw 'administrator rights required' }
  $done = $false
  $mgr = Get-TetheringManager
  if ($mgr) {
    try {
      if ($mgr.TetheringOperationalState.ToString() -eq 'On') {
        $r = Await ($mgr.StopTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
        if ($r.Status -eq 'Success') { $done = $true }
      }
    } catch { Add-Warning "WinRT stop failed: $($_.Exception.Message)" }
  }
  $hosted = Get-HostedNetworkState
  if ($null -ne $hosted -and (($hosted.status -match 'Started') -or ($hosted.status -eq $T_STARTED))) {
    & netsh.exe wlan stop hostednetwork 2>&1 | Out-Null
    $done = $true
  }
  $nics = Get-Nics
  $hotspot = Get-HotspotNic $nics
  $uplink = Get-UplinkNic $nics $hotspot
  if ($hotspot -and $uplink) {
    try { Set-IcsSharing $false $hotspot.name $uplink.name } catch { }
  }
  [ordered]@{ ok = $true; action = 'stop'; data = [ordered]@{ stopped = $done }; warnings = @($script:Warnings); error = $null }
}

function Invoke-Limit {
  if (-not (Test-Admin)) { throw 'administrator rights required' }
  if ([string]::IsNullOrWhiteSpace($Ip)) { throw 'IP is required' }

  foreach ($dir in 'UP', 'DOWN') {
    try { Remove-NetQosPolicy -Name "RW-$dir-$Ip" -Confirm:$false -ErrorAction Stop } catch { }
  }
  if ($UpKbps -gt 0) {
    New-NetQosPolicy -Name "RW-UP-$Ip" -IPSrcAddress $Ip -ThrottleRate ($UpKbps * 1024) -ErrorAction Stop | Out-Null
  }
  if ($DownKbps -gt 0) {
    New-NetQosPolicy -Name "RW-DOWN-$Ip" -IPDstAddress $Ip -ThrottleRate ($DownKbps * 1024) -ErrorAction Stop | Out-Null
  }
  [ordered]@{ ok = $true; action = 'limit'; data = [ordered]@{ ip = $Ip; upKbps = $UpKbps; downKbps = $DownKbps }; warnings = @($script:Warnings); error = $null }
}

function Invoke-Block {
  if (-not (Test-Admin)) { throw 'administrator rights required' }
  if ([string]::IsNullOrWhiteSpace($Ip)) { throw 'IP is required' }
  $name = "RedianWiFi-Block-$Ip"
  try { Remove-NetFirewallRule -DisplayName $name -ErrorAction Stop } catch { }
  try { Remove-NetFirewallRule -DisplayName "$name-IN" -ErrorAction Stop } catch { }
  New-NetFirewallRule -DisplayName $name -Direction Outbound -RemoteAddress $Ip -Action Block -Profile Any -ErrorAction Stop | Out-Null
  New-NetFirewallRule -DisplayName "$name-IN" -Direction Inbound -RemoteAddress $Ip -Action Block -Profile Any -ErrorAction Stop | Out-Null
  [ordered]@{ ok = $true; action = 'block'; data = [ordered]@{ ip = $Ip; blocked = $true }; warnings = @($script:Warnings); error = $null }
}

function Invoke-Unblock {
  if (-not (Test-Admin)) { throw 'administrator rights required' }
  try { Remove-NetFirewallRule -DisplayName "RedianWiFi-Block-$Ip" -ErrorAction Stop } catch { }
  try { Remove-NetFirewallRule -DisplayName "RedianWiFi-Block-$Ip-IN" -ErrorAction Stop } catch { }
  [ordered]@{ ok = $true; action = 'unblock'; data = [ordered]@{ ip = $Ip; blocked = $false }; warnings = @($script:Warnings); error = $null }
}

function Invoke-Stealth {
  if (-not (Test-Admin)) { throw 'administrator rights required' }
  $nics = Get-Nics
  $hotspot = Get-HotspotNic $nics
  if (-not $hotspot) { throw 'hotspot adapter not found (start the hotspot first)' }
  $result = [ordered]@{ ipv6 = $null; multicast = $null; ttl = 'unsupported' }

  if ($Ipv6 -ne 'keep') {
    $enable = ($Ipv6 -eq 'on')
    try {
      if ($enable) { Enable-NetAdapterBinding -Name $hotspot.name -ComponentID 'ms_tcpip6' -ErrorAction Stop }
      else { Disable-NetAdapterBinding -Name $hotspot.name -ComponentID 'ms_tcpip6' -ErrorAction Stop }
      $result.ipv6 = -not $enable
    } catch {
      Add-Warning "IPv6 toggle failed: $($_.Exception.Message)"
      $result.ipv6 = 'unknown'
    }
  }

  if ($Multicast -ne 'keep') {
    $on = ($Multicast -eq 'on')
    try {
      Get-NetFirewallRule -DisplayName 'RedianWiFi-Mcast-*' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue
      if ($on) {
        $prefix = $script:IcsSubnet
        foreach ($port in 5353, 5355, 137, 138) {
          New-NetFirewallRule -DisplayName "RedianWiFi-Mcast-$port" -Direction Outbound -Protocol UDP -RemotePort $port -RemoteAddress "$prefix.0/24" -Action Block -Profile Any -ErrorAction Stop | Out-Null
        }
      }
      $result.multicast = $on
    } catch {
      Add-Warning "multicast blocking failed: $($_.Exception.Message)"
      $result.multicast = 'unknown'
    }
  }

  $result.ttl = 'unsupported'
  [ordered]@{ ok = $true; action = 'stealth'; data = $result; warnings = @($script:Warnings); error = $null }
}

function Invoke-StealthReset {
  if (-not (Test-Admin)) { throw 'administrator rights required' }
  Get-NetFirewallRule -DisplayName 'RedianWiFi-Mcast-*' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue
  [ordered]@{ ok = $true; action = 'stealth-reset'; data = @{}; warnings = @($script:Warnings); error = $null }
}

function Invoke-Doctor {
  $nics = Get-Nics
  $hotspot = Get-HotspotNic $nics
  $uplink = Get-UplinkNic $nics $hotspot
  $hostedSupported = $null
  $drv = & netsh.exe wlan show drivers 2>&1 | Out-String
  if ($drv -match 'Hosted network supported\s*:\s*(\w+)') { $hostedSupported = $Matches[1] }

  [ordered]@{
    ok     = $true
    action = 'doctor'
    data   = [ordered]@{
      admin          = (Test-Admin)
      winrtTethering = $(if (Test-WinRtTethering) { 'available' } else { 'missing' })
      hostedNetwork  = $hostedSupported
      hotspotAdapter = $hotspot
      uplinkAdapter  = $uplink
      components     = [ordered]@{
        qos         = [bool](Get-Command New-NetQosPolicy -ErrorAction SilentlyContinue)
        firewall    = [bool](Get-Command New-NetFirewallRule -ErrorAction SilentlyContinue)
        adapterBind = [bool](Get-Command Set-NetAdapterBinding -ErrorAction SilentlyContinue)
        ttlDriver   = (Test-Path (Join-Path $PSScriptRoot '..\native\win\ttlfix.exe'))
      }
    }
    warnings = @($script:Warnings)
    error    = $null
  }
}

$payload = $null
try {
  switch ($Action) {
    'status'        { $payload = Invoke-Status -Detail $Detail }
    'doctor'        { $payload = Invoke-Doctor }
    'start'         { $payload = Invoke-Start }
    'stop'          { $payload = Invoke-Stop }
    'limit'         { $payload = Invoke-Limit }
    'block'         { $payload = Invoke-Block }
    'unblock'       { $payload = Invoke-Unblock }
    'stealth'       { $payload = Invoke-Stealth }
    'stealth-reset' { $payload = Invoke-StealthReset }
    'ttl'           { $payload = Invoke-Ttl }
    'name'          { $payload = Invoke-Name }
  }
} catch {
  $payload = [ordered]@{ ok = $false; action = $Action; data = $null; warnings = @($script:Warnings); error = $_.Exception.Message }
}

$payload | ConvertTo-Json -Compress -Depth 8