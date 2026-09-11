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
$output = @(& $Probe --api d3d11 2>&1)
$exit = $LASTEXITCODE
$output
if ($exit -ne 1 -or ($output -join "`n") -notmatch 'FAIL stage=expected-umd-identity-required ') {
    throw 'Missing UMD identity did not produce the intended rejection'
}
$output = @(& $Probe --api d3d12 --self-test-warp --expect-umd 'C:\bad.dll' 2>&1)
$exit = $LASTEXITCODE
$output
if ($exit -ne 1 -or ($output -join "`n") -notmatch 'FAIL stage=self-test-not-hardware-acceptance ') {
    throw 'WARP mixed with hardware identity did not produce the intended rejection'
}
# Both expected negative exits were checked above. Do not leak the final exit1
# into GitHub's outer PowerShell wrapper after the complete self-test passed.
$global:LASTEXITCODE = 0
'SELF_TESTS_ONLY_COMPLETE=1 TARGET_VIOGPU_TESTED=0'
