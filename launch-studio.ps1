# Launch the current Studio fork, keeping its required files together.
param(
    [switch]$SoftwareRenderer,
    [string]$BuildRoot = (Join-Path $PSScriptRoot 'build-studio-motion'),
    [string]$File
)
$ErrorActionPreference = 'Stop'
$release = Join-Path $BuildRoot 'build\Release'
$exe = Join-Path $release 'imgtool-studio.exe'
if (!(Test-Path -LiteralPath $exe)) { throw "Studio has not been built here. Run .\build-studio.ps1 first. Expected: $exe" }
if (!(Test-Path -LiteralPath (Join-Path $release 'SDL2.dll'))) { throw 'SDL2.dll is missing beside Studio. Run .\build-studio.ps1 to restore the complete build.' }
$arguments = @()
if ($File) {
    $resolvedFile = (Resolve-Path -LiteralPath $File).Path
    if ($resolvedFile.Contains('"')) { throw 'A file path cannot contain a double quote.' }
    $arguments += '"' + $resolvedFile + '"'
}
if ($SoftwareRenderer) { $arguments += '--software-renderer' }
Write-Host "Launching: $exe"
Write-Host "Startup log: $env:APPDATA\midway-imgtool\studio\startup.log"
$start = @{ FilePath = $exe; WorkingDirectory = $release; PassThru = $true }
if ($arguments.Count) { $start.ArgumentList = $arguments }
$process = Start-Process @start
if ($process.WaitForExit(3000)) {
    if ($process.ExitCode -ne 0) { throw "Studio exited with code $($process.ExitCode). See the startup log. Try .\launch-studio.ps1 -SoftwareRenderer to bypass accelerated graphics." }
} else {
    Write-Host "Studio is running (PID $($process.Id))."
}
