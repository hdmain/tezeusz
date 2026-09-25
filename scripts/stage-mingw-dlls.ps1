# Stage MinGW runtime DLLs next to seerr.exe so .\build\seerr.exe starts
# without requiring MinGW on PATH (avoids silent 0xC0000135 exit).
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$MingwBin = $env:MINGW_BIN
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $Exe)) { throw "missing exe: $Exe" }

$outDir = Split-Path -Parent (Resolve-Path $Exe)
if (-not $MingwBin) {
    foreach ($c in @(
        "C:\msys64\mingw64\bin",
        "C:\msys64\ucrt64\bin",
        "C:\msys64\clang64\bin"
    )) {
        if (Test-Path $c) { $MingwBin = $c; break }
    }
}
if (-not $MingwBin -or -not (Test-Path $MingwBin)) {
    Write-Host "stage-mingw-dlls: MinGW bin not found — skip"
    exit 0
}

$objdump = Join-Path $MingwBin "objdump.exe"
if (-not (Test-Path $objdump)) {
    Write-Host "stage-mingw-dlls: objdump missing — skip"
    exit 0
}

$skip = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
@(
    'kernel32.dll','user32.dll','gdi32.dll','shell32.dll','ole32.dll','oleaut32.dll','advapi32.dll',
    'winmm.dll','ws2_32.dll','wsock32.dll','iphlpapi.dll','dwmapi.dll','shlwapi.dll','psapi.dll',
    'version.dll','imm32.dll','oleacc.dll','comdlg32.dll','comctl32.dll','setupapi.dll','crypt32.dll',
    'bcrypt.dll','ncrypt.dll','secur32.dll','ntdll.dll','msvcrt.dll','ucrtbase.dll','opengl32.dll',
    'glu32.dll','winhttp.dll','wininet.dll','dnsapi.dll','mswsock.dll','rpcrt4.dll','msimg32.dll',
    'normaliz.dll','wldap32.dll','dbghelp.dll','userenv.dll','bcryptprimitives.dll','kernelbase.dll',
    'sechost.dll','gdi32full.dll','win32u.dll','msvcp_win.dll','cryptbase.dll','cfgmgr32.dll',
    'powrprof.dll','umpdc.dll','profapi.dll','wintrust.dll','imagehlp.dll','dxgi.dll','d3d11.dll',
    'd3d9.dll','d2d1.dll','dwrite.dll','hid.dll','devobj.dll','mpr.dll','nsi.dll','dhcpcsvc.dll'
) | ForEach-Object { [void]$skip.Add($_) }

$queue = [System.Collections.Generic.Queue[string]]::new()
$seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$queue.Enqueue((Resolve-Path $Exe).Path)

function Get-Imports([string]$file) {
    & $objdump -p $file 2>$null |
        Select-String -Pattern 'DLL Name:\s+(\S+)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
}

$copied = 0
while ($queue.Count -gt 0) {
    $cur = $queue.Dequeue()
    foreach ($name in (Get-Imports $cur)) {
        if ($skip.Contains($name)) { continue }
        if ($name -like 'api-ms-win-*' -or $name -like 'ext-ms-*') { continue }
        if (-not $seen.Add($name)) { continue }
        $src = Join-Path $MingwBin $name
        if (-not (Test-Path $src)) { continue }
        $dst = Join-Path $outDir $name
        Copy-Item -Force $src $dst
        $copied++
        $queue.Enqueue($dst)
    }
}

# OpenSSL 3 modules (libcrypto may need them)
$osslSrc = Join-Path (Split-Path $MingwBin -Parent) "lib\ossl-modules"
$osslDst = Join-Path $outDir "ossl-modules"
if (Test-Path $osslSrc) {
    New-Item -ItemType Directory -Force -Path $osslDst | Out-Null
    Copy-Item -Force (Join-Path $osslSrc "*") $osslDst
}

Write-Host "stage-mingw-dlls: copied $copied DLL(s) -> $outDir"
