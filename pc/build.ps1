$ErrorActionPreference = "Stop"

# Build the Windows sender (subscreen_sender.cpp).
#
# Deliberately does NOT use VsDevCmd.bat: that script shells out to reg.exe, which is
# blocked in restricted/sandboxed environments. Instead we locate the MSVC toolchain and
# the Windows SDK ourselves, compose INCLUDE/LIB, and invoke cl.exe directly.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI when there is
# no BOM, so non-ASCII comments would corrupt parsing.

$root = $PSScriptRoot
$src = Join-Path $root "subscreen_sender.cpp"
$exe = Join-Path $root "subscreen_sender.exe"
$obj = Join-Path $root "subscreen_sender.obj"

# /MT = statically link the C/C++ runtime. The exe then has ZERO non-system DLL
# dependencies, so double-clicking it works even when the VC++ redistributable is
# missing or when it is launched outside a developer shell. Without it the exe needs
# vcruntime140/msvcp140 and can fail with 0xc0000142 (STATUS_DLL_INIT_FAILED).
$cflags = @('/nologo', '/utf-8', '/std:c++17', '/EHsc', '/O2', '/MT', '/DNDEBUG', '/W3', '/arch:AVX2')

# ---- 1. locate cl.exe ----
$patterns = @(
  "$env:ProgramFiles\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe",
  "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe"
)
$cl = $null
foreach ($p in $patterns) {
  $found = Get-ChildItem -Path $p -ErrorAction SilentlyContinue | Sort-Object FullName -Descending
  if ($found) { $cl = $found | Select-Object -First 1; break }
}
if (-not $cl) { Write-Error "cl.exe not found. Install the C++ build tools of Visual Studio 2022."; exit 1 }

# cl.exe lives in <msvc>\bin\Hostx64\x64\, so three levels up is the toolset root.
$msvc = $cl.Directory.Parent.Parent.Parent.FullName
Write-Host "MSVC: $msvc"

# ---- 2. locate Windows SDK ----
$sdkRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
if (-not (Test-Path $sdkRoot)) { Write-Error "Windows SDK not found: $sdkRoot"; exit 1 }
$sdkInc = Get-ChildItem (Join-Path $sdkRoot "Include") -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
$sdkLib = Get-ChildItem (Join-Path $sdkRoot "Lib") -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkInc -or -not $sdkLib) { Write-Error "Windows SDK Include/Lib is incomplete"; exit 1 }
Write-Host "SDK : $($sdkInc.Name)"

# ---- 3. compose the build environment ----
$env:INCLUDE = @(
  (Join-Path $msvc "include"),
  (Join-Path $sdkInc.FullName "ucrt"),
  (Join-Path $sdkInc.FullName "shared"),
  (Join-Path $sdkInc.FullName "um"),
  (Join-Path $sdkInc.FullName "winrt"),
  (Join-Path $sdkInc.FullName "cppwinrt")
) -join ';'

$env:LIB = @(
  (Join-Path $msvc "lib\x64"),
  (Join-Path $sdkLib.FullName "ucrt\x64"),
  (Join-Path $sdkLib.FullName "um\x64")
) -join ';'

$env:PATH = "$($cl.Directory.FullName);$env:PATH"

# ---- 4. compile ----
# /SUBSYSTEM:WINDOWS = 纯 GUI 程序, 不再有常驻黑色控制台窗口。
# 入口仍是 main()(代码不变), 用 /ENTRY:mainCRTStartup 显式指定 CRT 入口。
& $cl.FullName $cflags "/Fe$exe" "/Fo$obj" $src "/link" "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup" "/MAP" "/MAPINFO:EXPORTS"
$code = $LASTEXITCODE

if ($code -ne 0) { Write-Error "build failed, exit code $code"; exit $code }
Write-Host ""
Write-Host "Build OK: $exe"
