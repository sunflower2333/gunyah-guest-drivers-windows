param([Parameter(Mandatory=$true)][string]$Probe)
$ErrorActionPreference = 'Stop'
$Probe = (Resolve-Path $Probe).Path
foreach ($api in @('d3d11','d3d12')) {
    $output = @(& $Probe --api $api --self-test-warp 2>&1)
    $exit = $LASTEXITCODE
    $output
    if ($exit -ne 0 -or ($output -join "`n") -notmatch 'HARNESS_SELF_TEST_PASS=') {
        throw "Microsoft runtime WARP probe self-test failed: $api"
    }
    if (($output -join "`n") -match 'NATIVE_RUNTIME.*PASS=') {
        throw 'A software self-test must not emit hardware acceptance'
    }
}
# Missing hardware identity must be rejected before trying a GPU. These cases
# catch accidental weakening of the ordinary-application acceptance contract.
& $Probe --api d3d11 2>&1 | Out-Host
if ($LASTEXITCODE -eq 0) { throw 'Missing UMD identity incorrectly accepted' }
& $Probe --api d3d12 --self-test-warp --expect-umd 'C:\bad.dll' 2>&1 | Out-Host
if ($LASTEXITCODE -eq 0) { throw 'WARP mixed with hardware identity incorrectly accepted' }
'SELF_TESTS_ONLY_COMPLETE=1 TARGET_VIOGPU_TESTED=0'
