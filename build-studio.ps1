# Rebuild the Studio preview using the existing Windows build infrastructure.
# From this repository: .\build-studio.ps1
# Output: build-studio-motion\build\Release\imgtool-studio.exe
param(
    [string]$BuildRoot = (Join-Path $PSScriptRoot 'build-studio-motion'),
    [string]$SharedDeps = "$env:LOCALAPPDATA\midway-build\deps"
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -SourceDir $PSScriptRoot -BuildRoot $BuildRoot -SharedDeps $SharedDeps -Sdl2Ver '2.32.10' -Arch x64
exit $LASTEXITCODE
