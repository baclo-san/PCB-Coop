<#
  build.ps1 — build the PCB co-op harness DLL + injector (32-bit / x86).

  th07.exe is a 32-bit PE, so EVERYTHING that touches its address space
  (the harness DLL, and the injector that does cross-process LoadLibrary)
  must be i686. We use the llvm-mingw i686 toolchain (clang+lld+mingw
  sysroot) — it ships headers and import libs, so no Windows SDK needed.

  Outputs:
    build\th07_harness.dll   (inject this into th07.exe)
    build\injector.exe       (launches th07.exe suspended + injects the DLL)
    build\harness.ini        (config template, only created if absent)

  Usage:
    powershell -ExecutionPolicy Bypass -File build.ps1
    powershell -File build.ps1 -Clean       # wipe build\ first
#>
param([switch]$Clean)

$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$build = Join-Path $root "build"
$obj   = Join-Path $build "obj"
$mh    = Join-Path $root "third_party\minhook"

function Find-Compiler {
    # 1) already on PATH?
    $c = Get-Command "i686-w64-mingw32-gcc.exe" -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    $c = Get-Command "i686-w64-mingw32-clang.exe" -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    # 2) winget package install location (llvm-mingw)
    $wg = Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages"
    if (Test-Path $wg) {
        $hit = Get-ChildItem $wg -Recurse -Filter "i686-w64-mingw32-gcc.exe" -ErrorAction SilentlyContinue |
               Select-Object -First 1
        if (-not $hit) {
            $hit = Get-ChildItem $wg -Recurse -Filter "i686-w64-mingw32-clang.exe" -ErrorAction SilentlyContinue |
                   Select-Object -First 1
        }
        if ($hit) { return $hit.FullName }
    }
    throw "32-bit toolchain not found. Install llvm-mingw:`n" +
          "  winget install --id MartinStorsjo.LLVM-MinGW.MSVCRT"
}

if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }
New-Item -ItemType Directory -Force -Path $obj | Out-Null

$CC = Find-Compiler
$CXX = $CC -replace "gcc\.exe$","g++.exe" -replace "clang\.exe$","clang++.exe"
Write-Host "Toolchain: $CC" -ForegroundColor Cyan

# common flags (the i686-w64-mingw32 wrapper already targets 32-bit; -m32 is belt-and-suspenders)
$cflags = @("-m32","-O2","-Wall")

# ---- 1. compile MinHook (C) ----
$mhSrcs = @(
    "src\buffer.c","src\hook.c","src\trampoline.c",
    "src\hde\hde32.c","src\hde\hde64.c"
)
$mhObjs = @()
foreach ($s in $mhSrcs) {
    $src = Join-Path $mh $s
    $o   = Join-Path $obj (([IO.Path]::GetFileNameWithoutExtension($s)) + ".o")
    Write-Host "  CC  $s"
    & $CC @cflags "-I$(Join-Path $mh 'include')" -c $src -o $o
    if ($LASTEXITCODE) { throw "compile failed: $s" }
    $mhObjs += $o
}

# ---- 2. harness DLL ----
$harnessSrc = Join-Path $root "src\harness\harness.c"
$harnessDll = Join-Path $build "th07_harness.dll"
Write-Host "  LD  th07_harness.dll"
& $CC @cflags "-I$(Join-Path $mh 'include')" -shared `
      $harnessSrc @mhObjs `
      -o $harnessDll `
      -static -lkernel32 -luser32
if ($LASTEXITCODE) { throw "link failed: th07_harness.dll" }

# ---- 2b. coop DLL (P2 entity graft + netcode integration, fork A §8) ----
# coop.c is C; the netcode core is C++. Compile coop.c to an object with the C
# compiler, then link it with the netcode TUs (via g++/clang++) so libstdc++ +
# winsock come in. Netplay is gated behind coop.ini [net] enabled=1; default-off
# leaves the local-keyboard co-op baseline unchanged.
$netcodeSrcs = @(
    (Join-Path $root "src\netplay\netcode.cpp"),
    (Join-Path $root "src\netplay\Connection.cpp"),
    (Join-Path $root "src\netplay\merge.cpp"),
    (Join-Path $root "src\netplay\netcode_c_api.cpp")
)
$netInc = "-I$(Join-Path $root 'src\netplay')"
$coopSrc = Join-Path $root "src\coop\coop.c"
$coopObj = Join-Path $obj  "coop.o"
$coopDll = Join-Path $build "th07_coop.dll"
Write-Host "  CC  coop.c"
& $CC @cflags "-I$(Join-Path $mh 'include')" $netInc -c $coopSrc -o $coopObj
if ($LASTEXITCODE) { throw "compile failed: coop.c" }
Write-Host "  LD  th07_coop.dll"
& $CXX @cflags "-std=c++17" $netInc -shared `
      $coopObj @netcodeSrcs @mhObjs `
      -o $coopDll `
      -static -lkernel32 -luser32 -lws2_32
if ($LASTEXITCODE) { throw "link failed: th07_coop.dll" }

# ---- 3. injector EXE ----
$injSrc = Join-Path $root "src\injector\injector.c"
$injExe = Join-Path $build "injector.exe"
Write-Host "  LD  injector.exe"
& $CC @cflags $injSrc -o $injExe -static -lkernel32
if ($LASTEXITCODE) { throw "link failed: injector.exe" }

# ---- 3b. netcode self-test (C++: validates STL + Winsock toolchain) ----
$netSrcs = @(
    (Join-Path $root "tests\netloop_test.cpp"),
    (Join-Path $root "src\netplay\netcode.cpp"),
    (Join-Path $root "src\netplay\Connection.cpp"),
    (Join-Path $root "src\netplay\merge.cpp"),
    (Join-Path $root "src\netplay\netcode_c_api.cpp")
)
$netTest = Join-Path $build "netloop_test.exe"
Write-Host "  CXX netloop_test.exe"
& $CXX @cflags "-Wextra" "-std=c++17" $netSrcs -o $netTest -static -lws2_32
if ($LASTEXITCODE) { throw "build failed: netloop_test.exe" }

# ---- 4. config template ----
$ini = Join-Path $build "harness.ini"
if (-not (Test-Path $ini)) {
@"
[harness]
; mode = record  -> pass real input through, write input_log.bin + sync_record.csv
; mode = replay  -> feed input_log.bin back in, write sync_replay_<pid>.csv
mode = record
; fixed RNG seed forced on first input poll (hex or decimal). Must match across
; both record and replay runs for the desync diff to be meaningful.
seed = 0x1234
"@ | Set-Content -Encoding ASCII $ini
    Write-Host "  GEN harness.ini"
}

$coopIni = Join-Path $build "coop.ini"
if (-not (Test-Path $coopIni)) {
@"
; th07_coop.dll config. [net] drives the built-in netplay (fork A §8). With
; enabled=0 (default) P2 is the local keyboard (IJKL/Space/U/O) — the
; confirmed-good local co-op baseline, byte-for-byte unchanged.
;
; Over the network: set enabled=1 on BOTH machines, role=host on one /
; role=guest on the other, give the guest the host's IP in peer=, and use the
; SAME delay and seed on both sides (a real seed handshake is a follow-up).

[coop]
; proximity_fade: when 1, fade the OTHER player out as they get close so your
; own character stays clear (host fades P2, guest fades P1; single-machine
; fades P2 near P1). Off by default; most meaningful under netplay.
proximity_fade = 0

[net]
enabled = 0
role  = host
peer  = 127.0.0.1
port  = 47000
local = 47001
delay = 2
seed  = 0x1234
"@ | Set-Content -Encoding ASCII $coopIni
    Write-Host "  GEN coop.ini"
}

# ---- 5. verify 32-bit ----
function Get-PEMachine([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    $m = [BitConverter]::ToUInt16($b, $pe + 4)
    switch ($m) { 0x14c { "x86 (32-bit) OK" } 0x8664 { "x64 (WRONG!)" } default { "0x{0:X}" -f $m } }
}
Write-Host ""
Write-Host "Built:" -ForegroundColor Green
foreach ($f in $harnessDll,$coopDll,$injExe) {
    "  {0,-22} {1,8:N0} bytes  {2}" -f (Split-Path $f -Leaf), (Get-Item $f).Length, (Get-PEMachine $f)
}
Write-Host ""
Write-Host "Next: copy build\th07_harness.dll + injector.exe + harness.ini together, then:" -ForegroundColor Yellow
Write-Host '  injector.exe "D:\Touhou 7 - Perfect Cherry Blossom\th07.exe"'
