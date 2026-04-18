param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build"

function Repair-DuplicateProcessEnvironmentNames {
    $environment = [System.Environment]::GetEnvironmentVariables("Process")
    $groups = @{}

    foreach ($key in $environment.Keys) {
        $normalized = $key.ToString().ToUpperInvariant()
        if (!$groups.ContainsKey($normalized)) {
            $groups[$normalized] = New-Object System.Collections.ArrayList
        }
        [void]$groups[$normalized].Add($key.ToString())
    }

    foreach ($group in $groups.GetEnumerator()) {
        if ($group.Value.Count -le 1) {
            continue
        }

        $preferred = $group.Value | Where-Object { $_ -ceq "Path" } | Select-Object -First 1
        if (!$preferred) {
            $preferred = $group.Value | Select-Object -First 1
        }

        $value = $environment[$preferred]
        foreach ($name in $group.Value) {
            if ($name -cne $preferred) {
                [System.Environment]::SetEnvironmentVariable($name, $null, "Process")
            }
        }

        [System.Environment]::SetEnvironmentVariable($preferred, $value, "Process")
        Write-Host "Normalized duplicate environment variable spellings for '$preferred'."
    }
}

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

function Normalize-PathForComparison {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $null
    }

    [char[]]$trimChars = @(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )

    return [System.IO.Path]::GetFullPath($Path).TrimEnd($trimChars)
}

function Get-CMakeCacheValue {
    param(
        [string]$CacheFile,
        [string]$Name
    )

    $pattern = "^$([regex]::Escape($Name)):[^=]+=(.*)$"
    $entry = Select-String -Path $CacheFile -Pattern $pattern |
        Select-Object -First 1

    if ($entry) {
        return $entry.Matches[0].Groups[1].Value
    }

    return $null
}

function Assert-SafeBuildDirectory {
    $repoRootFull = Normalize-PathForComparison $RepoRoot
    $buildDirFull = Normalize-PathForComparison $BuildDir
    $repoRootWithSeparator = $repoRootFull + [System.IO.Path]::DirectorySeparatorChar

    if ($buildDirFull -eq $repoRootFull -or
        !$buildDirFull.StartsWith($repoRootWithSeparator, [System.StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $buildDirFull) -ne "build") {
        throw "Refusing to remove unsafe build directory: $BuildDir"
    }
}

function Remove-DirectoryTreeWithRetry {
    param([string]$Path)

    $lastError = $null

    for ($attempt = 1; $attempt -le 3; $attempt++) {
        if (!(Test-Path -LiteralPath $Path)) {
            return
        }

        try {
            $items = Get-ChildItem -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue
            foreach ($item in $items) {
                try {
                    $item.Attributes = [System.IO.FileAttributes]::Normal
                } catch {
                    # Best effort only; Remove-Item may still succeed.
                }
            }

            try {
                $root = Get-Item -LiteralPath $Path -Force
                $root.Attributes = [System.IO.FileAttributes]::Normal
            } catch {
                # Best effort only; Remove-Item may still succeed.
            }

            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
            return
        } catch {
            $lastError = $_
            Start-Sleep -Milliseconds (250 * $attempt)
        }
    }

    throw $lastError
}

function Reset-CMakeBuildDirectory {
    param([string]$Reason)

    Assert-SafeBuildDirectory

    if (Test-Path $BuildDir) {
        Write-Host "Removing stale CMake build directory: $Reason"
        Remove-DirectoryTreeWithRetry -Path $BuildDir
        Write-Host "Removed: $BuildDir"
    }
}

function Get-CMakePath {
    $pathCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($pathCommand) {
        return $pathCommand.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installations = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
        foreach ($installation in $installations) {
            $candidate = Join-Path $installation "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }

    $candidates = @(
        (Join-Path $env:ProgramFiles "CMake\bin\cmake.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "CMake\bin\cmake.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    throw @"
CMake was not found.

Install Visual Studio Build Tools with the "Desktop development with C++" workload and the
"C++ CMake tools for Windows" component, or install CMake separately and put it on PATH.
"@
}

function Get-VisualStudioGenerator {
    param([string]$CMakePath)

    $helpText = & $CMakePath --help
    $knownGenerators = @(
        "Visual Studio 17 2022",
        "Visual Studio 16 2019"
    )

    foreach ($generator in $knownGenerators) {
        if ($helpText -match [regex]::Escape($generator)) {
            return $generator
        }
    }

    throw @"
No supported Visual Studio CMake generator was found.

Install Visual Studio Build Tools 2022 or 2019 with the "Desktop development with C++" workload.
"@
}

Repair-DuplicateProcessEnvironmentNames

$cmake = Get-CMakePath
$generator = Get-VisualStudioGenerator -CMakePath $cmake

$cacheFile = Join-Path $BuildDir "CMakeCache.txt"
if (Test-Path $cacheFile) {
    $cacheProblems = New-Object System.Collections.ArrayList

    $cachedSourceDir = Get-CMakeCacheValue -CacheFile $cacheFile -Name "CMAKE_HOME_DIRECTORY"
    if ($cachedSourceDir -and
        (Normalize-PathForComparison $cachedSourceDir) -ne (Normalize-PathForComparison $RepoRoot)) {
        [void]$cacheProblems.Add("source was '$cachedSourceDir'")
    }

    $cachedBuildDir = Get-CMakeCacheValue -CacheFile $cacheFile -Name "CMAKE_CACHEFILE_DIR"
    if ($cachedBuildDir -and
        (Normalize-PathForComparison $cachedBuildDir) -ne (Normalize-PathForComparison $BuildDir)) {
        [void]$cacheProblems.Add("build directory was '$cachedBuildDir'")
    }

    $cachedGenerator = Get-CMakeCacheValue -CacheFile $cacheFile -Name "CMAKE_GENERATOR"

    if ($cachedGenerator -and $cachedGenerator -ne $generator) {
        [void]$cacheProblems.Add("generator was '$cachedGenerator'")
    }

    $cachedPlatform = Get-CMakeCacheValue -CacheFile $cacheFile -Name "CMAKE_GENERATOR_PLATFORM"
    if ($cachedPlatform -and $cachedPlatform -ne "x64") {
        [void]$cacheProblems.Add("platform was '$cachedPlatform'")
    }

    if ($cacheProblems.Count -gt 0) {
        Reset-CMakeBuildDirectory -Reason ($cacheProblems -join "; ")
    }
}

Write-Host "Repository: $RepoRoot"
Write-Host "Build type: $Configuration"
Write-Host "Using CMake: $cmake"
Write-Host "Generator: $generator"

Invoke-NativeCommand $cmake @("-S", $RepoRoot, "-B", $BuildDir, "-G", $generator, "-A", "x64")
Invoke-NativeCommand $cmake @("--build", $BuildDir, "--config", $Configuration)

$exePath = Join-Path $BuildDir "$Configuration\ClassicNotepad.exe"
if (!(Test-Path $exePath)) {
    throw "Build finished, but the expected executable was not found: $exePath"
}

Write-Host "Built: $exePath"
