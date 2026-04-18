param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build"

function Invoke-NativeCommand {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE`: $FilePath $($Arguments -join ' ')"
    }
}

function Get-CTestPath {
    $pathCommand = Get-Command ctest -ErrorAction SilentlyContinue
    if ($pathCommand) {
        return $pathCommand.Source
    }

    $cacheFile = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path $cacheFile) {
        $cmakePath = Select-String -Path $cacheFile -Pattern "^CMAKE_COMMAND:INTERNAL=(.+)$" |
            Select-Object -First 1 |
            ForEach-Object { $_.Matches[0].Groups[1].Value }

        if ($cmakePath) {
            $candidate = Join-Path (Split-Path -Parent $cmakePath) "ctest.exe"
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installations = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
        foreach ($installation in $installations) {
            $candidate = Join-Path $installation "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }

    $candidates = @(
        (Join-Path $env:ProgramFiles "CMake\bin\ctest.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "CMake\bin\ctest.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw @"
CTest was not found.

Build first with .\scripts\build.ps1, or install CMake and add it to PATH.
"@
}

$ctest = Get-CTestPath

Write-Host "Repository: $RepoRoot"
Write-Host "Build type: $Configuration"
Write-Host "Using CTest: $ctest"

Invoke-NativeCommand $ctest @("--test-dir", $BuildDir, "-C", $Configuration, "--output-on-failure")
