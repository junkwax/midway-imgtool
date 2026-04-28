# build.ps1 — Build midway-imgtool (Windows/SDL2 port) using VS 2022
# Run from a regular Windows PowerShell window:
#   powershell -ExecutionPolicy Bypass -File build.ps1
#
# Source lives in WSL at /home/alex/midway-imgtool
# This script accesses it via \\wsl.localhost\Ubuntu\...

param(
    [string]$SourceDir  = $PSScriptRoot,
    [string]$BuildRoot  = "$env:LOCALAPPDATA\imgtool-build",
    [string]$SharedDeps = "$env:LOCALAPPDATA\midway-build\deps",
    [string]$Sdl2Ver    = "",          # leave empty to auto-fetch latest SDL2 2.x
    [string]$Arch       = "x64"        # x64 (default) or x86
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Validate architecture
if ($Arch -eq "x86") {
    $cmakeArch = "Win32"
    $sdl2Lib   = "x86"
    $vcvarsArch = "x86"
} else {
    if ($Arch -ne "x64") { Write-Warning "Unknown arch '$Arch', using x64" }
    $Arch = "x64"
    $cmakeArch = "x64"
    $sdl2Lib   = "x64"
    $vcvarsArch = "x64"
}

# -----------------------------------------------------------------------
# 1. Locate VS 2022
# -----------------------------------------------------------------------
$vsRoot = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($path -and (Test-Path "$path\VC\Auxiliary\Build\vcvarsall.bat")) {
        $vsRoot = $path
    }
}
$vsRoots = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
)
if (-not $vsRoot) {
    $vsRoot = $vsRoots | Where-Object { Test-Path "$_\VC\Auxiliary\Build\vcvarsall.bat" } | Select-Object -First 1
}
if (-not $vsRoot) { Write-Error "VS 2022 not found"; exit 1 }
$vcvarsall = "$vsRoot\VC\Auxiliary\Build\vcvarsall.bat"
$cmakeRel  = "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$vsCmake   = ($vsRoots | ForEach-Object { "$_\$cmakeRel" } | Where-Object { Test-Path $_ } | Select-Object -First 1)
if ($vsCmake) { $vsCmake = Split-Path $vsCmake } else { $vsCmake = "" }
Write-Host "[1/4] Found VS 2022 at $vsRoot" -ForegroundColor Cyan
if ($vsCmake) { Write-Host "      cmake: $vsCmake" -ForegroundColor Cyan } else { Write-Host "      cmake not found in VS installs, relying on PATH" -ForegroundColor Yellow }

# -----------------------------------------------------------------------
# 2. Resolve SDL2 version
# -----------------------------------------------------------------------
if (-not $Sdl2Ver) {
    Write-Host "[2/4] Querying GitHub for latest SDL2 2.x release..." -ForegroundColor Cyan
    try {
        $releases = Invoke-RestMethod "https://api.github.com/repos/libsdl-org/SDL/releases?per_page=20"
        $r = $releases | Where-Object { $_.tag_name -match '^release-2\.' } | Select-Object -First 1
        $Sdl2Ver = $r.tag_name -replace '^release-', ''
        Write-Host "    Latest SDL2: $Sdl2Ver"
    } catch {
        $Sdl2Ver = "2.30.2"
        Write-Host "    GitHub query failed, using fallback SDL2 $Sdl2Ver"
    }
} else {
    Write-Host "[2/4] Using SDL2 $Sdl2Ver" -ForegroundColor Cyan
}

$sdl2Root  = "$SharedDeps\SDL2-$Sdl2Ver"
$sdl2Cmake = "$sdl2Root\cmake"
$buildDir  = "$BuildRoot\build"

New-Item -ItemType Directory -Force -Path $BuildRoot  | Out-Null
New-Item -ItemType Directory -Force -Path $SharedDeps | Out-Null
New-Item -ItemType Directory -Force -Path $buildDir   | Out-Null

# -----------------------------------------------------------------------
# 3. SDL2 (shared with bddview — stored in midway-build\deps)
# -----------------------------------------------------------------------
if (-not (Test-Path $sdl2Root)) {
    $url = "https://github.com/libsdl-org/SDL/releases/download/release-$Sdl2Ver/SDL2-devel-$Sdl2Ver-VC.zip"
    $zip = "$SharedDeps\sdl2.zip"
    Write-Host "[3/4] Downloading SDL2-devel-$Sdl2Ver-VC.zip ..." -ForegroundColor Cyan
    (New-Object Net.WebClient).DownloadFile($url, $zip)
    Write-Host "      Extracting..."
    Expand-Archive -Path $zip -DestinationPath $SharedDeps -Force
    Remove-Item $zip
} else {
    Write-Host "[3/4] SDL2 $Sdl2Ver already present (shared)" -ForegroundColor Cyan
}

if (-not (Test-Path $sdl2Cmake)) {
    Write-Error "SDL2 cmake dir not found at $sdl2Cmake - please check extraction"
    exit 1
}

# -----------------------------------------------------------------------
# 4. CMake configure + build via a temp batch (inherits vcvarsall env)
# -----------------------------------------------------------------------
Write-Host "[4/4] Configuring and building ($Arch Release)..." -ForegroundColor Cyan

$lines = @(
    "@echo off",
    "call \`"$vcvarsall\`" $vcvarsArch",
    "if errorlevel 1 exit /b 1",
    "set PATH=$vsCmake;%PATH%",
    "cmake -B \`"$buildDir\`" -G \`"Visual Studio 17 2022\`" -A $cmakeArch -DSDL2_DIR=\`"$sdl2Cmake\`" \`"$SourceDir\`"",
    "if errorlevel 1 exit /b 1",
    "cmake --build `"$buildDir`" --config Release",
    "if errorlevel 1 exit /b 1"
)
$batFile = "$env:TEMP\build_imgtool.bat"
[System.IO.File]::WriteAllLines($batFile, $lines, [System.Text.Encoding]::ASCII)
& cmd.exe /c $batFile
$exitCode = $LASTEXITCODE

if ($exitCode -eq 0) {
    $exe = "$buildDir\Release\imgtool.exe"
    Write-Host ""
    Write-Host "*** Build succeeded ***" -ForegroundColor Green
    Write-Host "EXE: $exe" -ForegroundColor Green
    # SDL2.dll must be next to the exe
    $sdl2Dll = "$sdl2Root\lib\$sdl2Lib\SDL2.dll"
    if (Test-Path $sdl2Dll) {
        Copy-Item $sdl2Dll (Split-Path $exe) -Force
        Write-Host "Copied SDL2.dll to output folder" -ForegroundColor Green
    }
    # it.hlp is loaded by pressing 'h' in the app; it must sit next to the exe
    $hlp = Join-Path $SourceDir "IT\it.hlp"
    if (Test-Path $hlp) {
        Copy-Item $hlp (Split-Path $exe) -Force
        Write-Host "Copied it.hlp to output folder" -ForegroundColor Green
    }
    # DMA2.txt hardware reference
    $dma2 = Join-Path $SourceDir "DMA2.txt"
    if (Test-Path $dma2) {
        Copy-Item $dma2 (Split-Path $exe) -Force
        Write-Host "Copied DMA2.txt to output folder" -ForegroundColor Green
    }
} else {
    Write-Host ""
    Write-Host "*** Build FAILED (exit $exitCode) ***" -ForegroundColor Red
}

exit $exitCode
