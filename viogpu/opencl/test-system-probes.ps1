param([string]$Probes = 'opencl-system-probes',
      [string]$Loaders = 'opencl-probe-ci-loader-support',
      [string]$Evidence = 'opencl-probe-launch-evidence')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path $Probes).Path
$loadersRoot = (Resolve-Path $Loaders).Path
$out = New-Item -ItemType Directory ([IO.Path]::GetFullPath($Evidence))
$receipt = Get-Content "$root/opencl-system-probes-receipt.json" -Raw | ConvertFrom-Json
$oldPath = $env:PATH
$results = @()
try {
    foreach ($arch in @('arm64','x64','x86')) {
        $name = "viogpu-opencl-check-$arch.exe"
        if ((Get-FileHash "$root/$name" -Algorithm SHA256).Hash -ne $receipt.files.$name.sha256) {
            throw "Probe hash mismatch: $arch"
        }
        $app = New-Item -ItemType Directory "$($out.FullName)/ordinary-$arch"
        Copy-Item "$root/$name" $app.FullName
        # CI has no VIOGPU driver. A separate PATH directory supplies the genuine
        # public loader solely to prove process launch; no ICD or CRT is staged.
        # Target acceptance must use its normally installed system OpenCL.dll.
        $view = if ($arch -eq 'x86') {'x86'} else {'arm64x'}
        $env:PATH = "$loadersRoot/$view;$oldPath"
        $stdout = "$($out.FullName)/$arch.stdout.txt"
        $stderr = "$($out.FullName)/$arch.stderr.txt"
        $process = Start-Process -FilePath "$($app.FullName)/$name" -WorkingDirectory $app.FullName -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if (!$process.WaitForExit(45000)) {
            $process.Kill()
            throw "Probe launch timed out: $arch"
        }
        $process.Refresh()
        $output = Get-Content $stdout -Raw
        $errorOutput = Get-Content $stderr -Raw -ErrorAction SilentlyContinue
        Write-Host $output
        Write-Host $errorOutput
        if ($output -notmatch 'pointer_bits=(32|64) pid=\d+' -or $output -notmatch '(?im)^module OpenCL\.dll=.+OpenCL\.dll\s*$') {
            throw "Probe did not reach main with public OpenCL loader: $arch exit=$($process.ExitCode)"
        }
        if ($process.ExitCode -ne 0 -and $process.ExitCode -ne 1) {
            throw "Unexpected exception exit: $arch exit=$($process.ExitCode)"
        }
        if (@(Get-ChildItem $app.FullName -File).Count -ne 1) {
            throw "Ordinary application directory must contain only its EXE: $arch"
        }
        $results += [ordered]@{architecture=$arch; sha256=$receipt.files.$name.sha256;
            process_launch='PASS'; exit_code=$process.ExitCode; stdout=$output;
            stderr=$errorOutput; private_dll_siblings=0; gpu_execution_claimed=$false}
        Write-Host "PASS $arch ordinary EXE reached main/public loader; no GPU execution claimed"
    }
} finally {
    $env:PATH = $oldPath
    [ordered]@{schema=1; scope='CI startup only, public loader supplied by separate PATH directory; no VIOGPU installed';
        ci=$env:GITHUB_RUN_ID; probe_parent=$receipt.probe_build.parent; results=$results} |
        ConvertTo-Json -Depth 8 | Set-Content "$($out.FullName)/launch-receipt.json"
}
