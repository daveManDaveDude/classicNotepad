param(
    [string]$Distro = "Ubuntu-24.04",
    [string]$BuildDir = "build-ubuntu",
    [int]$Parallel = 2,
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

$configure = "cd '$RepoRootForWsl' && cmake -S . -B '$BuildDir' -G 'Unix Makefiles'"
$build = "cd '$RepoRootForWsl' && cmake --build '$BuildDir' --parallel $Parallel"
$test = "cd '$RepoRootForWsl' && ctest --test-dir '$BuildDir' --output-on-failure"

Write-Host "Repository: $RepoRoot"
Write-Host "WSL distro: $Distro"
Write-Host "Ubuntu build dir: $BuildDir"

Invoke-Wsl $configure
Invoke-Wsl $build

if (!$SkipTests) {
    Invoke-Wsl $test
}
