#requires -Version 5.1
<#
.SYNOPSIS
    Build + deploy WraithEngine: Wraith.dll/d3d9.dll/WraithPatcher.exe (32-bit) and
    WraithHost.exe (64-bit).

.PARAMETER Config
    Build configuration. Default: Release.

.PARAMETER ClientPath
    Client directory to deploy into. Optional once it has been cached by a first run.

.PARAMETER Target
    What to build: All (default), Dll (32-bit shim), or Host (64-bit translator).

.PARAMETER Clean
    Delete the build directory before configuring (forces a from-scratch build).

.PARAMETER AutoPatch
    After the build, run WraithPatcher on the client's Wow.exe. The patcher is idempotent
    (it skips an already-patched exe and backs the original up to Wow.exe.orig on first run).

.EXAMPLE
    .\build.ps1
.EXAMPLE
    .\build.ps1 -Target Dll
.EXAMPLE
    .\build.ps1 -ClientPath "D:\Path\To\Client" -Clean
.EXAMPLE
    .\build.ps1 -AutoPatch
#>
param(
    [string]$Config = "Release",
    [string]$ClientPath,
    [ValidateSet("All", "Dll", "Host")]
    [string]$Target = "All",
    [switch]$Clean,
    [switch]$AutoPatch
)

$ErrorActionPreference = "Stop"

$root      = $PSScriptRoot
$buildDll  = Join-Path $root "build\dll"
$buildHost = Join-Path $root "build\host"

if (-not (Test-Path (Join-Path $root "CMakeLists.txt"))) {
    throw "CMakeLists.txt is missing from '$root'. Place build.ps1 in the project root directory (next to CMakeLists.txt)."
}

function Get-CachedClientPath([string]$cacheDir) {
    $cache = Join-Path $cacheDir "CMakeCache.txt"
    if (Test-Path $cache) {
        $hit = Select-String -Path $cache -Pattern '^CLIENT_PATH:PATH=(.+)$' | Select-Object -First 1
        if ($hit) { return $hit.Matches[0].Groups[1].Value.Trim() }
    }
    return $null
}

if (-not $ClientPath) {
    $ClientPath = Get-CachedClientPath $buildDll
    if (-not $ClientPath) { $ClientPath = Get-CachedClientPath $buildHost }
}
if (-not $ClientPath) {
    throw "No client path is known. Run the command once with -ClientPath '<client folder>' (it will be stored in the CMake cache)."
}
if (-not (Test-Path $ClientPath)) {
    throw "Client path not found: $ClientPath"
}

Write-Host "Project : $root"
Write-Host "Client : $ClientPath"
Write-Host "Config : $Config"
Write-Host ""

function Invoke-Native([string]$exe, [string[]]$cmdArgs) {
    Write-Host ">> $exe $($cmdArgs -join ' ')" -ForegroundColor Cyan
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $exe @cmdArgs } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "Failure ($LASTEXITCODE): $exe $($cmdArgs -join ' ')" }
}

function Build-Variant([string]$buildDir, [string]$arch, [string]$label) {
    Write-Host "=== $label ===" -ForegroundColor Green

    if ($Clean -and (Test-Path $buildDir)) {
        Write-Host "Clean $buildDir" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $buildDir
    }

    $needConfigure = $Clean `
        -or $PSBoundParameters.ContainsKey('ClientPath') `
        -or (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt")))
    if ($needConfigure) {
        Invoke-Native "cmake" @("-S", $root, "-B", $buildDir, "-A", $arch, "-DCLIENT_PATH=$ClientPath")
    }

    Invoke-Native "cmake" @("--build", $buildDir, "--config", $Config, "--parallel")
    Write-Host ""
}

function Invoke-AutoPatch {
    $patcher = Join-Path $ClientPath "WraithPatcher.exe"
    if (-not (Test-Path $patcher)) { $patcher = Join-Path $buildDll "Release\WraithPatcher.exe" }
    if (-not (Test-Path $patcher)) {
        throw "WraithPatcher.exe not found. Build the Dll target first (.\build.ps1 -Target Dll)."
    }
    $wow = Join-Path $ClientPath "Wow.exe"
    if (-not (Test-Path $wow)) { throw "Wow.exe not found: $wow" }

    Write-Host "=== AutoPatch ===" -ForegroundColor Green
    Invoke-Native $patcher @($wow)
    Write-Host ""
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()

if ($Target -eq "All" -or $Target -eq "Dll")  { Build-Variant $buildDll  "Win32" "Wraith.dll (32-bit)" }
if ($Target -eq "All" -or $Target -eq "Host") { Build-Variant $buildHost "x64"   "WraithHost.exe (64-bit)" }
if ($AutoPatch) { Invoke-AutoPatch }

$sw.Stop()
Write-Host "OK - build + deploy in $([int]$sw.Elapsed.TotalSeconds)s -> $ClientPath" -ForegroundColor Green
