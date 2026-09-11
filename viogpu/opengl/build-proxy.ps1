param([string]$Output = 'opengl-system')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path '.').Path
$source = Join-Path $root 'viogpu/opengl'
$mesa = Join-Path $root 'external/mesa'
$out = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force $out | Out-Null
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC installation not found' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$exports = @(Get-Content "$source/proxy-exports.txt" | ForEach-Object {
    $parts = $_ -split ' '; @{ Name = $parts[0]; Bytes = $parts[1] }
})
function Invoke-Compiler([string]$arch, [string[]]$commands) {
    $script = Join-Path $out "build-$arch.cmd"
    @('@echo off', "call `"$dev`" -no_logo -host_arch=x64 -arch=$arch", 'if errorlevel 1 exit /b 1') +
        @($commands | ForEach-Object { $_; 'if errorlevel 1 exit /b 1' }) |
        Set-Content $script -Encoding ascii
    & cmd.exe /d /c $script
    if ($LASTEXITCODE) { throw "MSVC $arch build failed" }
}
foreach ($arch in @('arm64','x64','x86')) {
    $def = Join-Path $out "proxy-$arch.def"
    @('EXPORTS') + @($exports | ForEach-Object {
        if ($arch -eq 'x86') { "$($_.Name)=_$($_.Name)@$($_.Bytes)" } else { $_.Name }
    }) | Set-Content $def -Encoding ascii
    Invoke-Compiler $arch @(
        "cl /nologo /W4 /WX /EHsc /MT /LD /I`"$mesa/include`" /I`"$mesa/src/gallium/frontends/wgl`" `"$source/icd-proxy.cpp`" /Fo`"$out/proxy-$arch.obj`" /link /DEF:`"$def`" /OUT:`"$out/viogpuopengl_$arch.dll`" /PDB:`"$out/viogpuopengl_$arch.pdb`" /DEBUG",
        "cl /nologo /W4 /WX /EHsc /MT /I`"$mesa/include`" `"$source/system-probe.cpp`" /Fo`"$out/probe-$arch.obj`" /Fe:`"$out/system-probe-$arch.exe`" user32.lib gdi32.lib opengl32.lib"
    )
}
foreach ($arch in @('arm64','x64')) {
    @('EXPORTS') + @($exports | ForEach-Object { "$($_.Name)=viogpuopengl_$arch.$($_.Name)" }) |
        Set-Content "$out/forward-$arch.def" -Encoding ascii
}
Invoke-Compiler arm64 @(
    "cl /nologo /c /Fo`"$out/empty-arm64.obj`" `"$source/empty.cpp`"",
    "cl /nologo /c /arm64EC /Fo`"$out/empty-ec.obj`" `"$source/empty.cpp`"",
    "link /lib /machine:arm64 /def:`"$out/forward-arm64.def`" /out:`"$out/forward-arm64.lib`"",
    "link /lib /machine:x64 /def:`"$out/forward-x64.def`" /out:`"$out/forward-x64.lib`"",
    "link /dll /noentry /machine:arm64x /defArm64Native:`"$out/forward-arm64.def`" /def:`"$out/forward-x64.def`" `"$out/empty-arm64.obj`" `"$out/empty-ec.obj`" `"$out/forward-arm64.lib`" `"$out/forward-x64.lib`" /out:`"$out/viogpuopengl.dll`""
)
