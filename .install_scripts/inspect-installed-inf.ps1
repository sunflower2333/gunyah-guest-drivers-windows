$ErrorActionPreference='Stop'
$d = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' })[0]
$p = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Driver'
$k = "Registry::HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\$($p.Data)"
$i = Get-Item -LiteralPath $k
foreach ($n in @('DriverVersion','InfPath','InfSection','MatchingDeviceId','DriverDate','ProviderName')) {
    "{0} = {1}" -f $n, $i.GetValue($n, '<unset>')
}
$inf = $i.GetValue('InfPath','')
if ($inf) {
    $full = Join-Path $env:SystemRoot "INF\$inf"
    if (Test-Path $full) {
        "--- $full ---"
        Select-String -Path $full -Pattern 'DriverVer' | ForEach-Object { $_.Line.Trim() }
    }
}
"--- oem INFs mentioning viogpuwddm ---"
Get-ChildItem "$env:SystemRoot\INF\oem*.inf" -ErrorAction SilentlyContinue | ForEach-Object {
    $t = Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue
    if ($t -match 'viogpuwddm') {
        $v = ([regex]::Match($t, 'DriverVer\s*=\s*[^,]+,\s*([0-9.]+)')).Groups[1].Value
        "{0} -> {1}" -f $_.Name, $v
    }
}
