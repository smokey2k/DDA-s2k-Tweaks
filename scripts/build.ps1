param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$cmake = Get-Command cmake -ErrorAction SilentlyContinue

if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        foreach ($visualStudio in (& $vswhere -all -products '*' -property installationPath)) {
            $bundledCmake = Join-Path $visualStudio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $bundledCmake) {
                $cmake = Get-Item -LiteralPath $bundledCmake
                break
            }
        }
    }
}

if (-not $cmake) {
    throw 'CMake was not found in PATH or in Visual Studio. Install CMake 3.20 or newer.'
}

$cmakePath = if ($cmake.Source) { $cmake.Source } else { $cmake.FullName }

& $cmakePath -S $projectRoot -B "$projectRoot\build" -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

& $cmakePath --build "$projectRoot\build" --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
