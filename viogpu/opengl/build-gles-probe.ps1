param([string]$Output = 'gles-probes')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path '.').Path
$out = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force $out | Out-Null
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC installation not found' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$manifest = [ordered]@{ probe_commit = (& git rev-parse HEAD).Trim(); mesa_commit = (& git -C external/mesa rev-parse HEAD).Trim(); files = @{} }
foreach ($arch in @('arm64','x64','x86')) {
    $exe = Join-Path $out "gles-probe-$arch.exe"
    $script = Join-Path $env:RUNNER_TEMP "gles-probe-$arch.cmd"
    @('@echo off', "call `"$dev`" -no_logo -host_arch=x64 -arch=$arch", 'if errorlevel 1 exit /b 1',
      "cl /nologo /W4 /WX /EHsc /MT /I`"$root/external/mesa/include`" `"$root/viogpu/opengl/gles-probe.cpp`" /Fo`"$env:RUNNER_TEMP/gles-probe-$arch.obj`" /Fe:`"$exe`" user32.lib gdi32.lib", 'exit /b %errorlevel%') |
        Set-Content $script -Encoding ascii
    try {
        & cmd.exe /d /c $script
        if ($LASTEXITCODE) { throw "GLES probe $arch compilation failed" }
    } finally { Remove-Item -LiteralPath $script -ErrorAction SilentlyContinue }
    $bytes = [IO.File]::ReadAllBytes($exe)
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    $machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
    $expected = @{ arm64 = 0xaa64; x64 = 0x8664; x86 = 0x14c }[$arch]
    if ($machine -ne $expected) { throw "GLES probe $arch PE machine mismatch" }
    $manifest.files["gles-probe-$arch.exe"] = @{ sha256 = (Get-FileHash $exe -Algorithm SHA256).Hash; machine = $machine }
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $out 'probe-identity.json') -Encoding utf8
