param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Debug",
    [ValidateSet("Ordinary", "Stress", "Failure", "Full")]
    [string]$Suite = "Ordinary"
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
    $script:LastNativeExitCode = $LASTEXITCODE
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

$script:LastNativeExitCode = 0
Invoke-NativeCommand $ctest @("--test-dir", $BuildDir, "-C", $Configuration, "--output-on-failure")
$ctestExitCode = $script:LastNativeExitCode
if ($ctestExitCode -ne 0) {
    exit $ctestExitCode
}

$binary = Join-Path $BuildDir "$Configuration\ClassicNotepad.exe"
if (!(Test-Path -LiteralPath $binary)) {
    $binary = Join-Path $BuildDir "ClassicNotepad.exe"
}
if (!(Test-Path -LiteralPath $binary)) {
    throw "The $Configuration ClassicNotepad.exe was not found under $BuildDir. Build first with .\scripts\build.ps1."
}

$pythonCommand = Get-Command python -ErrorAction SilentlyContinue
if (!$pythonCommand) {
    throw "Python was not found on PATH; install Python 3 to run the Windows automation suite."
}

$runner = Join-Path $RepoRoot "tests\automation\run_automation_tests.py"
Write-Host "Automation suite: $Suite"
$script:LastNativeExitCode = 0
Invoke-NativeCommand $pythonCommand.Source @(
    $runner,
    "--binary", $binary,
    "--platform", "windows",
    "--suite", $Suite
)
$automationExitCode = $script:LastNativeExitCode
if ($automationExitCode -ne 0) {
    exit $automationExitCode
}
