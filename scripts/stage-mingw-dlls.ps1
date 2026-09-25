# Stage MinGW runtime DLLs next to seerr.exe (local convenience).
# Always exits 0 so CI/link is never blocked — portable packaging uses bash.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$MingwBin = $env:MINGW_BIN
)

$ErrorActionPreference = "Continue"

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Host "stage-mingw-dlls: missing exe - skip"
    exit 0
}

$outDir = Split-Path -Parent (Resolve-Path -LiteralPath $Exe)

if (-not $MingwBin -and $env:MINGW_PREFIX) {
    $cand = Join-Path $env:MINGW_PREFIX "bin"
    if (Test-Path -LiteralPath $cand) { $MingwBin = $cand }
}

if (-not $MingwBin) {
    foreach ($c in @(
        "C:\msys64\mingw64\bin",
        "C:\msys64\ucrt64\bin",
        "C:\msys64\clang64\bin",
        "D:\a\_temp\msys64\mingw64\bin"
    )) {
        if (Test-Path -LiteralPath $c) { $MingwBin = $c; break }
    }
}

if (-not $MingwBin) {
    $cmd = Get-Command objdump.exe -ErrorAction SilentlyContinue
    if ($cmd) { $MingwBin = Split-Path -Parent $cmd.Source }
}

if (-not $MingwBin -or -not (Test-Path -LiteralPath $MingwBin)) {
    Write-Host "stage-mingw-dlls: MinGW bin not found - skip"
    exit 0
}

$objdump = Join-Path $MingwBin "objdump.exe"
if (-not (Test-Path -LiteralPath $objdump)) {
    Write-Host "stage-mingw-dlls: objdump missing - skip"
    exit 0
}

$skipNames = @(
    'kernel32.dll','user32.dll','gdi32.dll','shell32.dll','ole32.dll','oleaut32.dll','advapi32.dll',
    'winmm.dll','ws2_32.dll','wsock32.dll','iphlpapi.dll','dwmapi.dll','shlwapi.dll','psapi.dll',
    'version.dll','imm32.dll','oleacc.dll','comdlg32.dll','comctl32.dll','setupapi.dll','crypt32.dll',
    'bcrypt.dll','ncrypt.dll','secur32.dll','ntdll.dll','msvcrt.dll','ucrtbase.dll','opengl32.dll',
    'glu32.dll','winhttp.dll','wininet.dll','dnsapi.dll','mswsock.dll','rpcrt4.dll','msimg32.dll',
    'normaliz.dll','wldap32.dll','dbghelp.dll','userenv.dll','bcryptprimitives.dll','kernelbase.dll',
    'sechost.dll','gdi32full.dll','win32u.dll','msvcp_win.dll','cryptbase.dll','cfgmgr32.dll',
    'powrprof.dll','umpdc.dll','profapi.dll','wintrust.dll','imagehlp.dll','dxgi.dll','d3d11.dll',
    'd3d9.dll','d2d1.dll','dwrite.dll','hid.dll','devobj.dll','mpr.dll','nsi.dll','dhcpcsvc.dll'
)
$skip = @{}
foreach ($n in $skipNames) { $skip[$n.ToLowerInvariant()] = $true }

$queue = New-Object System.Collections.Generic.Queue[string]
$seen = @{}
$queue.Enqueue((Resolve-Path -LiteralPath $Exe).Path)

$copied = 0
while ($queue.Count -gt 0) {
    $cur = $queue.Dequeue()
    $raw = & cmd.exe /c "`"$objdump`" -p `"$cur`" 2>nul"
    if (-not $raw) { continue }
    foreach ($line in $raw) {
        if ($line -notmatch 'DLL Name:\s+(\S+)') { continue }
        $name = $Matches[1]
        if ([string]::IsNullOrWhiteSpace($name)) { continue }
        $key = $name.ToLowerInvariant()
        if ($skip.ContainsKey($key)) { continue }
        if ($key.StartsWith('api-ms-win-') -or $key.StartsWith('ext-ms-')) { continue }
        if ($seen.ContainsKey($key)) { continue }
        $seen[$key] = $true
        $src = Join-Path $MingwBin $name
        if (-not (Test-Path -LiteralPath $src)) { continue }
        $dst = Join-Path $outDir $name
        Copy-Item -Force -LiteralPath $src -Destination $dst -ErrorAction SilentlyContinue
        $copied++
        $queue.Enqueue($dst)
    }
}

$osslSrc = Join-Path (Split-Path $MingwBin -Parent) "lib\ossl-modules"
$osslDst = Join-Path $outDir "ossl-modules"
if (Test-Path -LiteralPath $osslSrc) {
    New-Item -ItemType Directory -Force -Path $osslDst | Out-Null
    Copy-Item -Force (Join-Path $osslSrc "*") $osslDst -ErrorAction SilentlyContinue
}

Write-Host "stage-mingw-dlls: copied $copied DLL(s) -> $outDir"
exit 0
