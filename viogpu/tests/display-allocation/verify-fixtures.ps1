$ErrorActionPreference = 'Stop'
$expected = @{
    'absent.bin' = '072abc86168f91ce65846b7dd686386e669e45a38d98149d3a6677caeccea71a'
    'bar8m_reserved8m_align16k.bin' = '090c011994286a84ab31c0f2f45f34ca258b0b9819debb26b4dfdddc4e5c74f2'
    'bar128m_reserved8m_align4k.bin' = '4eff18e63b7df0830d80fb3cf7f646c716ad47b296bfdc2a0a0264998ce20664'
    'bar128m_reserved8m_align16k.bin' = '62d08d8d41cfdb18ae7e5c18d448eabe45f3fdde9565eb6f1d905c420a434092'
}
foreach ($name in $expected.Keys) {
    $path = Join-Path $PSScriptRoot "fixtures/$name"
    if ((Get-Item $path).Length -ne 128 -or (Get-FileHash $path -Algorithm SHA256).Hash -ne $expected[$name]) {
        throw "Actual host fixture changed: $name"
    }
}
$header = Join-Path $PSScriptRoot '../../shared/dvsa_protocol.h'
if ((Get-FileHash $header -Algorithm SHA256).Hash -ne '41e1b2cc133cf23622d4d78f410eb8497496e41badb49a4f6005512ae0ba3525') {
    throw 'Canonical host wire header changed without a coordinated version update'
}
Write-Host 'PASS canonical host header and four actual encoded response identities'
