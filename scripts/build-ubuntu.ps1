param(
    [string]$Distro = "Ubuntu-24.04",
    [string]$BuildDir = "build-ubuntu",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Debug",
    [int]$Parallel = 2,
    [ValidateSet("Auto", "Ninja", "Unix Makefiles")]
    [string]$Generator = "Auto",
    [switch]$Fresh,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ($RepoRoot -notmatch '^([A-Za-z]):(.*)$') {
    throw "This script expects the repository to be on a Windows drive path."
}

$drive = $Matches[1].ToLowerInvariant()
$tail = $Matches[2] -replace '\\', '/'
$RepoRootForWsl = "/mnt/$drive$tail"

function Invoke-Wsl {
    param([string]$Command)

    wsl.exe -d $Distro -- bash -lc $Command
    if ($LASTEXITCODE -ne 0) {
        throw "WSL command failed with exit code $LASTEXITCODE`: $Command"
    }
}

function Test-Wsl {
    param([string]$Command)

    wsl.exe -d $Distro -- bash -lc $Command | Out-Null
    return $LASTEXITCODE -eq 0
}

function Get-WslOutput {
    param([string]$Command)

    $output = wsl.exe -d $Distro -- bash -lc $Command
    if ($LASTEXITCODE -ne 0) {
        throw "WSL command failed with exit code $LASTEXITCODE`: $Command"
    }

    return ($output -join "`n").Trim()
}

$selectedGenerator = $Generator
if ($selectedGenerator -eq "Auto") {
    if (Test-Wsl "command -v ninja >/dev/null 2>&1 || command -v ninja-build >/dev/null 2>&1") {
        $selectedGenerator = "Ninja"
    } else {
        $selectedGenerator = "Unix Makefiles"
    }
}

$existingGenerator = Get-WslOutput "cd '$RepoRootForWsl' && if [ -f '$BuildDir/CMakeCache.txt' ]; then sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' '$BuildDir/CMakeCache.txt' | head -n 1; fi"
if ($existingGenerator -and $existingGenerator -ne $selectedGenerator) {
    if ($Fresh) {
        Write-Host "Fresh reconfigure requested: switching Ubuntu build dir from '$existingGenerator' to '$selectedGenerator'."
    } else {
        Write-Warning "Ubuntu build dir '$BuildDir' is already configured with '$existingGenerator'. Using it for this build; pass -Fresh to reconfigure with '$selectedGenerator'."
        $selectedGenerator = $existingGenerator
    }
}

$freshConfigure = ""
$freshPrefix = ""
if ($Fresh) {
    if (Test-Wsl "cmake --help | grep -q -- '--fresh'") {
        $freshConfigure = " --fresh"
    } else {
        $freshPrefix = "cmake -E rm -rf '$BuildDir/CMakeCache.txt' '$BuildDir/CMakeFiles' && "
    }
}

$configure = "cd '$RepoRootForWsl' && $freshPrefix" + "cmake$freshConfigure -S . -B '$BuildDir' -G '$selectedGenerator'"
$configure += " -DCMAKE_BUILD_TYPE='$Configuration'"
$build = "cd '$RepoRootForWsl' && cmake --build '$BuildDir' --parallel $Parallel"
$test = "cd '$RepoRootForWsl' && ctest --test-dir '$BuildDir' --output-on-failure"

Write-Host "Repository: $RepoRoot"
Write-Host "WSL distro: $Distro"
Write-Host "Ubuntu build dir: $BuildDir"
Write-Host "Build type: $Configuration"
Write-Host "CMake generator: $selectedGenerator"

Invoke-Wsl $configure
Invoke-Wsl $build

if (!$SkipTests) {
    Invoke-Wsl $test
}
