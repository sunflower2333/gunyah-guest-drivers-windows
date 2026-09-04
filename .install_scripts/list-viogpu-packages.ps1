$ErrorActionPreference='Continue'
Get-ChildItem (Join-Path $env:SystemRoot 'INF') -Filter 'oem*.inf' -ErrorAction SilentlyContinue | ForEach-Object {
    $t = Get-Content $_.FullName -Raw -ErrorAction SilentlyContinue
    if ($t -match 'viogpuwddm') {
        $v = ([regex]::Match($t, 'DriverVer\s*=\s*[^,]+,\s*([0-9.]+)')).Groups[1].Value
        "{0} -> {1}" -f $_.Name, $v
    }
} | Sort-Object
