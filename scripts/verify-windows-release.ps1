param(
    [ValidateSet("Ordinary", "Stress", "Failure", "Full")]
    [string]$Suite = "Ordinary",
    [string]$CMakePath,
    [string]$CTestPath,
    [string]$PythonPath,
    [string]$VsWherePath
)

$ErrorActionPreference = "Stop"
$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$BuildDir = Join-Path $RepoRoot "build\windows-release-verification"
$EvidenceDir = Join-Path $RepoRoot "build\windows-release-verification-evidence"
$VersionFile = Join-Path $RepoRoot "VERSION"
$Runner = Join-Path $RepoRoot "tests\automation\run_automation_tests.py"
$script:StepResults = New-Object System.Collections.ArrayList
$script:ExitCode = 0
$script:SelectedGenerator = $null
$script:BinaryHash = $null
$script:FailureMessage = $null
$script:VersionRestored = $false
$script:VersionUnexpectedMutation = $false
$script:EvidenceDirectorySafe = $false
$script:AutomationResultPath = $null
$script:AutomationResult = $null
$script:AutomationAttempted = $false
$script:AutomationChildExitCode = $null
$script:AutomationValidation = $null
$script:ManifestObject = $null
$script:ManifestPath = [System.IO.Path]::Combine((Split-Path -Parent $Runner), "test_suite_manifest.json")
$script:ManifestSha256 = $null
$script:ToolInfo = [ordered]@{}
$script:InstalledInstances = @()
$startedAt = [DateTime]::UtcNow
$script:RunTag = $startedAt.ToString("yyyyMMddTHHmmssZ") + "-$PID"
$versionExisted = Test-Path -LiteralPath $VersionFile
$versionSnapshot = $null
$versionHashBefore = $null

function Get-Sha256Bytes {
    param([byte[]]$Bytes)

    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [System.BitConverter]::ToString($algorithm.ComputeHash($Bytes)).Replace("-", "")
    } finally {
        $algorithm.Dispose()
    }
}

function Get-CommandPath {
    param([string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command -and $command.Source) {
        return $command.Source
    }
    return $null
}

function Assert-SafeRepositoryDirectory {
    param(
        [string]$RepositoryRoot,
        [string]$TargetPath,
        [string]$Description
    )

    $repositoryFull = [System.IO.Path]::GetFullPath($RepositoryRoot).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    $targetFull = [System.IO.Path]::GetFullPath($TargetPath).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    $rootItem = Get-Item -LiteralPath $repositoryFull -Force -ErrorAction Stop
    if (($rootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing $Description because repository root is a reparse point: $repositoryFull"
    }
    $prefix = $repositoryFull + [System.IO.Path]::DirectorySeparatorChar
    if ($targetFull.Equals($repositoryFull, [System.StringComparison]::OrdinalIgnoreCase) -or
        !$targetFull.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing $Description outside repository root: $targetFull"
    }

    $relative = $targetFull.Substring($prefix.Length)
    $parts = $relative -split '[\\/]'
    $current = $repositoryFull
    foreach ($part in $parts) {
        if ([string]::IsNullOrWhiteSpace($part)) { continue }
        $current = Join-Path $current $part
        $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if (!$item) { continue }
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing $Description because path component is a reparse point: $current"
        }
    }

    $resolvedRoot = (Resolve-Path -LiteralPath $repositoryFull -ErrorAction Stop).ProviderPath.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    $existing = $targetFull
    while (!(Test-Path -LiteralPath $existing)) {
        $parent = Split-Path -Parent $existing
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $existing) { break }
        $existing = $parent
    }
    $resolvedExisting = (Resolve-Path -LiteralPath $existing -ErrorAction Stop).ProviderPath.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    $resolvedPrefix = $resolvedRoot + [System.IO.Path]::DirectorySeparatorChar
    if ($resolvedExisting -ne $resolvedRoot -and
        !$resolvedExisting.StartsWith($resolvedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing $Description because its resolved parent escapes repository root: $resolvedExisting"
    }
    return $targetFull
}

function Assert-SafeBuildTree {
    param([string]$BuildPath)

    if (!(Test-Path -LiteralPath $BuildPath -PathType Container)) { return }
    $resolvedBuild = (Resolve-Path -LiteralPath $BuildPath -ErrorAction Stop).ProviderPath.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar
    )
    $resolvedPrefix = $resolvedBuild + [System.IO.Path]::DirectorySeparatorChar
    $pending = New-Object System.Collections.Stack
    $pending.Push($BuildPath)
    while ($pending.Count -gt 0) {
        $directory = [string]$pending.Pop()
        try {
            foreach ($entry in [System.IO.Directory]::EnumerateFileSystemEntries($directory)) {
                $attributes = [System.IO.File]::GetAttributes($entry)
                if (($attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                    $resolvedEntry = $entry
                    try { $resolvedEntry = (Resolve-Path -LiteralPath $entry -ErrorAction Stop).ProviderPath }
                    catch { $resolvedEntry = "<unresolved>" }
                    $resolvedEntryFull = if ($resolvedEntry -ne "<unresolved>") { [System.IO.Path]::GetFullPath($resolvedEntry) } else { $resolvedEntry }
                    $insideBuild = $resolvedEntryFull -eq $resolvedBuild -or
                        $resolvedEntryFull.StartsWith($resolvedPrefix, [System.StringComparison]::OrdinalIgnoreCase)
                    if (!$insideBuild) {
                        throw "Refusing build cache because descendant reparse point resolves outside it: $entry -> $resolvedEntryFull"
                    }
                    throw "Refusing build cache because descendant reparse points are not allowed: $entry -> $resolvedEntryFull"
                }
                if (($attributes -band [System.IO.FileAttributes]::Directory) -ne 0) {
                    $pending.Push($entry)
                }
            }
        } catch {
            if ($_.Exception.Message -match "Refusing build cache because descendant reparse point") { throw }
            throw "Could not inspect dedicated build tree '$directory': $($_.Exception.Message)"
        }
    }
}

function Get-VsWherePath {
    if ($VsWherePath) { return $VsWherePath }
    $candidate = Get-CommandPath "vswhere"
    if ($candidate) { return $candidate }
    $candidate = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $candidate) { return $candidate }
    throw "vswhere.exe was not found; an installed Visual Studio instance is required for the Release gate."
}

function Get-CMakePath {
    if ($CMakePath) { return $CMakePath }
    $candidate = Get-CommandPath "cmake"
    if ($candidate) { return $candidate }
    foreach ($installation in $script:InstalledInstances) {
        $candidate = Join-Path $installation.installationPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    foreach ($candidate in @(
        (Join-Path $env:ProgramFiles "CMake\bin\cmake.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "CMake\bin\cmake.exe")
    )) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw "CMake was not found. Install CMake or Visual Studio C++ CMake tools."
}

function Get-CTestPath {
    param([string]$ResolvedCMake)

    if ($CTestPath) { return $CTestPath }
    $candidate = Get-CommandPath "ctest"
    if ($candidate) { return $candidate }
    $candidate = Join-Path (Split-Path -Parent $ResolvedCMake) "ctest.exe"
    if (Test-Path -LiteralPath $candidate) { return $candidate }
    throw "CTest was not found. Install CMake with CTest or add it to PATH."
}

function Get-PythonPath {
    if ($PythonPath) { return $PythonPath }
    $candidate = Get-CommandPath "python"
    if ($candidate) { return $candidate }
    throw "Python 3 was not found. Install Python 3 and add it to PATH."
}

function Invoke-ExternalStep {
    param(
        [string]$Name,
        [string]$FilePath,
        [string[]]$Arguments
    )

    if (!$script:EvidenceDirectorySafe) {
        throw "Evidence directory has not passed its containment check; refusing to run $Name."
    }
    [void](Assert-SafeRepositoryDirectory -RepositoryRoot $RepoRoot -TargetPath $EvidenceDir -Description "write step logs")
    $logStem = ($Name -replace '[^A-Za-z0-9_.-]', '_')
    $stdoutLog = Join-Path $EvidenceDir "$($script:RunTag)-$logStem.stdout.log"
    $stderrLog = Join-Path $EvidenceDir "$($script:RunTag)-$logStem.stderr.log"
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    $childExitCode = 1
    if (!(Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        [System.IO.File]::WriteAllText($stdoutLog, "", (New-Object System.Text.UTF8Encoding($false)))
        [System.IO.File]::WriteAllText($stderrLog, "Executable not found: $FilePath", (New-Object System.Text.UTF8Encoding($false)))
    } else {
        $priorErrorActionPreference = $ErrorActionPreference
        try {
            # Windows PowerShell can surface native stderr lines as NativeCommandError.
            # They are diagnostic output, not a gate failure unless the child exit code is nonzero.
            $ErrorActionPreference = "Continue"
            & $FilePath @Arguments 1> $stdoutLog 2> $stderrLog
            $childExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $priorErrorActionPreference
        }
    }
    $timer.Stop()
    $stdoutText = [System.IO.File]::ReadAllText($stdoutLog)
    $stderrText = [System.IO.File]::ReadAllText($stderrLog)
    $step = [ordered]@{
        name = $Name
        executable = $FilePath
        arguments = $Arguments
        exitCode = $childExitCode
        elapsedMilliseconds = [Math]::Round($timer.Elapsed.TotalMilliseconds, 1)
        stdoutLog = $stdoutLog
        stderrLog = $stderrLog
    }
    [void]$script:StepResults.Add($step)
    $script:LastExternalStep = [pscustomobject]@{
        name = $Name
        executable = $FilePath
        exitCode = $childExitCode
        stdoutLog = $stdoutLog
        stderrLog = $stderrLog
        stdoutText = $stdoutText
        stderrText = $stderrText
    }
    Write-Host "$Name exit=$childExitCode; stdout=$stdoutLog; stderr=$stderrLog"
    if ($childExitCode -ne 0) {
        $exception = New-Object System.InvalidOperationException("$Name failed with exit code $childExitCode")
        $exception.Data["ChildExitCode"] = $childExitCode
        throw $exception
    }
    return $script:LastExternalStep
}

function Get-CacheValue {
    param([string]$CacheFile, [string]$Name)

    $entry = Select-String -LiteralPath $CacheFile -Pattern "^$([regex]::Escape($Name)):[^=]+=(.*)$" |
        Select-Object -First 1
    if ($entry) { return $entry.Matches[0].Groups[1].Value }
    return $null
}

function Normalize-Path {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    [char[]]$trimChars = @([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    return [System.IO.Path]::GetFullPath($Path).TrimEnd($trimChars)
}

function Get-GeneratorForInstallation {
    param([object]$Installation)

    $major = 0
    if ($Installation.installationVersion -match '^(\d+)\.') { $major = [int]$Matches[1] }
    switch ($major) {
        18 { return "Visual Studio 18 2026" }
        17 { return "Visual Studio 17 2022" }
        16 { return "Visual Studio 16 2019" }
        default { return $null }
    }
}

function Get-AdvertisedGenerators {
    param([string]$HelpText)

    return @("Visual Studio 18 2026", "Visual Studio 17 2022", "Visual Studio 16 2019") |
        Where-Object { $HelpText.Contains($_) }
}

function Get-SupportedGenerator {
    param([string[]]$AdvertisedGenerators)

    foreach ($installation in $script:InstalledInstances) {
        $generator = Get-GeneratorForInstallation $installation
        if ($generator -and $generator -in $AdvertisedGenerators) { return $generator }
    }
    throw "CMake advertises no supported generator backed by an installed Visual Studio C++ x64 instance."
}

function Get-InstalledVisualStudioInstances {
    param([string]$VsWhere)

    $step = Invoke-ExternalStep "vswhere-inventory" $VsWhere @(
        "-all", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-format", "json"
    )
    try {
        $instances = ConvertFrom-Json -InputObject $step.stdoutText
    } catch {
        throw "vswhere returned invalid JSON: $($_.Exception.Message)"
    }
    if ($null -eq $instances) { throw "vswhere reported no installed Visual Studio C++ instances." }
    $sorted = @($instances | Sort-Object { [version]$_.installationVersion } -Descending)
    if ($sorted.Count -eq 0) { throw "vswhere reported no installed Visual Studio C++ instances." }
    foreach ($instance in $sorted) {
        if (!$instance.installationPath -or !$instance.installationVersion) {
            throw "vswhere returned an instance without installationPath or installationVersion."
        }
    }
    return $sorted
}

function Get-ToolVersionText {
    param([string]$Name, [string]$Path, [string[]]$Arguments = @("--version"))

    $step = Invoke-ExternalStep "$Name-version" $Path $Arguments
    $text = ($step.stdoutText + "`n" + $step.stderrText).Trim()
    if ([string]::IsNullOrWhiteSpace($text)) { return "(version command returned no text)" }
    return (($text -split "`r?`n")[0]).Trim()
}

function Validate-AutomationResult {
    $errors = New-Object System.Collections.ArrayList
    if (!$script:AutomationResultPath -or !(Test-Path -LiteralPath $script:AutomationResultPath -PathType Leaf)) {
        [void]$errors.Add("structured runner result was not produced")
    } else {
        try {
            $result = Get-Content -LiteralPath $script:AutomationResultPath -Raw | ConvertFrom-Json
            $script:AutomationResult = $result
            $expectedIds = @($script:ManifestObject.suites.$Suite)
            $selectedIds = @($result.selectedIds | ForEach-Object { [string]$_ })
            $executedIds = @($result.executedIds | ForEach-Object { [string]$_ })
            $outcomeIds = @($result.outcomes | ForEach-Object { [string]$_.testId })
            $expectedText = [string]::Join("`n", [string[]]$expectedIds)
            if ([string]::Join("`n", [string[]]$selectedIds) -cne $expectedText) {
                [void]$errors.Add("runner selected IDs differ from the $Suite manifest selection")
            }
            if ([string]::Join("`n", [string[]]$executedIds) -cne $expectedText) {
                [void]$errors.Add("executed test IDs differ from the selected IDs")
            }
            if ([string]::Join("`n", [string[]]$outcomeIds) -cne $expectedText) {
                [void]$errors.Add("outcome IDs differ from the selected IDs")
            }
            if ($result.manifestSha256 -cne $script:ManifestSha256) { [void]$errors.Add("runner manifest hash differs from gate manifest hash") }
            if ($result.resultPath -and ([System.IO.Path]::GetFullPath([string]$result.resultPath) -ne [System.IO.Path]::GetFullPath($script:AutomationResultPath))) {
                [void]$errors.Add("runner result path differs from the requested evidence path")
            }
            if ($result.runnerSha256 -cne (Get-Sha256Bytes ([System.IO.File]::ReadAllBytes($Runner)))) { [void]$errors.Add("runner source hash does not match the executed runner") }
            if ($result.binarySha256 -cne $script:BinaryHash) { [void]$errors.Add("runner binary hash differs from gate binary hash") }
            if ($null -ne $script:AutomationChildExitCode -and [int]$result.exitCode -ne $script:AutomationChildExitCode) {
                [void]$errors.Add("runner JSON exit code differs from process exit code")
            }

            $counts = $result.counts
            $observed = @{
                passed = @($result.outcomes | Where-Object { $_.status -eq "passed" }).Count
                failed = @($result.outcomes | Where-Object { $_.status -eq "failed" }).Count
                errors = @($result.outcomes | Where-Object { $_.status -eq "error" }).Count
                skipped = @($result.outcomes | Where-Object { $_.status -eq "skipped" }).Count
                expectedFailures = @($result.outcomes | Where-Object { $_.status -eq "expectedFailure" }).Count
                unexpectedSuccesses = @($result.outcomes | Where-Object { $_.status -eq "unexpectedSuccess" }).Count
            }
            if ([int]$counts.selected -ne $expectedIds.Count) { [void]$errors.Add("runner selected count differs from manifest count") }
            if ([int]$counts.executed -ne $outcomeIds.Count) { [void]$errors.Add("runner executed count differs from outcome count") }
            foreach ($name in $observed.Keys) {
                if ([int]$counts.$name -ne $observed[$name]) { [void]$errors.Add("runner $name count differs from outcome records") }
            }
            if (($observed.Values | Measure-Object -Sum).Sum -ne $outcomeIds.Count) {
                [void]$errors.Add("outcome statuses do not account for every executed ID")
            }
        } catch {
            [void]$errors.Add("could not validate structured runner result: $($_.Exception.Message)")
        }
    }

    $script:AutomationValidation = [ordered]@{
        valid = ($errors.Count -eq 0)
        errors = @($errors)
        resultPath = $script:AutomationResultPath
        selectedCount = if ($script:AutomationResult) { @($script:AutomationResult.selectedIds).Count } else { 0 }
        executedCount = if ($script:AutomationResult) { @($script:AutomationResult.executedIds).Count } else { 0 }
        counts = if ($script:AutomationResult) { $script:AutomationResult.counts } else { $null }
    }
    if ($errors.Count -gt 0 -and $script:ExitCode -eq 0) {
        $script:ExitCode = 1
        $script:FailureMessage = "Automation evidence reconciliation failed: $($errors -join '; ')"
    }
}

function Assert-CompatibleCache {
    param(
        [string]$CacheFile,
        [string]$ExpectedSource,
        [string]$ExpectedBuild,
        [string[]]$AdvertisedGenerators
    )

    $actualSource = Get-CacheValue $CacheFile "CMAKE_HOME_DIRECTORY"
    $actualBuild = Get-CacheValue $CacheFile "CMAKE_CACHEFILE_DIR"
    $actualGenerator = Get-CacheValue $CacheFile "CMAKE_GENERATOR"
    $actualInstance = Get-CacheValue $CacheFile "CMAKE_GENERATOR_INSTANCE"
    $actualPlatform = Get-CacheValue $CacheFile "CMAKE_GENERATOR_PLATFORM"
    $problems = New-Object System.Collections.ArrayList
    if ((Normalize-Path $actualSource) -ne (Normalize-Path $ExpectedSource)) { [void]$problems.Add("source directory '$actualSource'") }
    if ((Normalize-Path $actualBuild) -ne (Normalize-Path $ExpectedBuild)) { [void]$problems.Add("build directory '$actualBuild'") }
    if ($actualGenerator -notin $AdvertisedGenerators) { [void]$problems.Add("generator '$actualGenerator' is not advertised by CMake") }
    $matchingInstances = @($script:InstalledInstances | Where-Object { (Get-GeneratorForInstallation $_) -eq $actualGenerator })
    if ($matchingInstances.Count -eq 0) { [void]$problems.Add("generator '$actualGenerator' has no installed Visual Studio instance") }
    if (![string]::IsNullOrWhiteSpace($actualInstance)) {
        $normalizedInstance = Normalize-Path $actualInstance
        $knownInstances = @($matchingInstances | ForEach-Object { Normalize-Path $_.installationPath })
        if ($normalizedInstance -notin $knownInstances) {
            [void]$problems.Add("generator instance '$actualInstance' is not present in the installed instance inventory")
        }
    } elseif ($matchingInstances.Count -ne 1) {
        [void]$problems.Add("generator '$actualGenerator' has no recorded instance and resolves to $($matchingInstances.Count) installed instances")
    }
    if ($actualPlatform -ne "x64") { [void]$problems.Add("platform '$actualPlatform'") }
    $configurations = Get-CacheValue $CacheFile "CMAKE_CONFIGURATION_TYPES"
    if ($configurations -notmatch '(^|;)Release($|;)') { [void]$problems.Add("Release configuration is unavailable") }
    if ($problems.Count -gt 0) {
        throw "Dedicated Release cache is incompatible ($($problems -join ', ')); it was retained at $BuildDir."
    }
    return $actualGenerator
}

function Get-SourceIdentity {
    $identity = [ordered]@{ revision = $null; dirty = $null }
    if (!(Test-Path -LiteralPath (Join-Path $RepoRoot ".git"))) { return $identity }
    $git = Get-CommandPath "git"
    if (!$git) { return $identity }
    $revision = & $git -C $RepoRoot rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0) { $identity.revision = ($revision | Out-String).Trim() }
    $status = & $git -C $RepoRoot status --porcelain 2>$null
    if ($LASTEXITCODE -eq 0) { $identity.dirty = [bool](@($status).Count -gt 0) }
    return $identity
}

try {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw "This verification gate requires Windows and the Visual Studio x64 generator."
    }
    [void](Assert-SafeRepositoryDirectory -RepositoryRoot $RepoRoot -TargetPath $EvidenceDir -Description "write gate evidence")
    $script:EvidenceDirectorySafe = $true
    [void](Assert-SafeRepositoryDirectory -RepositoryRoot $RepoRoot -TargetPath $BuildDir -Description "use the dedicated Release cache")
    [void](Assert-SafeBuildTree -BuildPath $BuildDir)
    if (!(Test-Path -LiteralPath $EvidenceDir)) { [void](New-Item -ItemType Directory -Path $EvidenceDir -Force) }
    [void](Assert-SafeRepositoryDirectory -RepositoryRoot $RepoRoot -TargetPath $EvidenceDir -Description "write gate evidence")

    if (!$versionExisted) { throw "VERSION was not present at gate startup: $VersionFile" }
    $versionSnapshot = [System.IO.File]::ReadAllBytes($VersionFile)
    $versionHashBefore = Get-Sha256Bytes $versionSnapshot

    $manifestBytes = [System.IO.File]::ReadAllBytes($script:ManifestPath)
    $script:ManifestSha256 = Get-Sha256Bytes $manifestBytes
    $script:ManifestObject = ConvertFrom-Json -InputObject ([System.Text.Encoding]::UTF8.GetString($manifestBytes))
    if ($script:ManifestObject.schemaVersion -ne 1 -or
        $script:ManifestObject.discoveryPattern -ne "test_*.py" -or
        $null -eq $script:ManifestObject.suites.PSObject.Properties[$Suite]) {
        throw "Automation suite manifest is invalid or does not define $Suite."
    }

    $vswhere = Get-VsWherePath
    $vswhereVersion = Get-ToolVersionText "vswhere" $vswhere @("-?")
    $script:ToolInfo.vswhere = [ordered]@{ path = $vswhere; version = $vswhereVersion }
    $script:InstalledInstances = Get-InstalledVisualStudioInstances -VsWhere $vswhere
    $script:ToolInfo.visualStudioInstances = @($script:InstalledInstances | ForEach-Object {
        [ordered]@{ installationPath = $_.installationPath; installationVersion = $_.installationVersion }
    })
    $cmake = Get-CMakePath
    $ctest = Get-CTestPath -ResolvedCMake $cmake
    $python = Get-PythonPath
    $script:ToolInfo.cmake = [ordered]@{ path = $cmake; version = Get-ToolVersionText "cmake" $cmake }
    $script:ToolInfo.ctest = [ordered]@{ path = $ctest; version = Get-ToolVersionText "ctest" $ctest }
    $script:ToolInfo.python = [ordered]@{ path = $python; version = Get-ToolVersionText "python" $python }
    $helpStep = Invoke-ExternalStep "cmake-generator-help" $cmake @("--help")
    $script:AdvertisedGenerators = Get-AdvertisedGenerators -HelpText $helpStep.stdoutText
    $generator = Get-SupportedGenerator -AdvertisedGenerators $script:AdvertisedGenerators

    Write-Host "Repository: $RepoRoot"
    Write-Host "Dedicated build cache: $BuildDir"
    Write-Host "Suite: $Suite"
    Write-Host "Generator: $generator (x64)"

    $cacheFile = Join-Path $BuildDir "CMakeCache.txt"
    if (Test-Path -LiteralPath $BuildDir) {
        if (Test-Path -LiteralPath $cacheFile) {
            $generator = Assert-CompatibleCache -CacheFile $cacheFile -ExpectedSource $RepoRoot -ExpectedBuild $BuildDir -AdvertisedGenerators $script:AdvertisedGenerators
        } else {
            $entries = @(Get-ChildItem -LiteralPath $BuildDir -Force)
            if ($entries.Count -gt 0) {
                throw "Dedicated build path exists without CMakeCache.txt and was retained: $BuildDir"
            }
        }
    }
    $script:SelectedGenerator = $generator

    if (!(Test-Path -LiteralPath $cacheFile)) {
        [void](Assert-SafeRepositoryDirectory -RepositoryRoot $RepoRoot -TargetPath $BuildDir -Description "configure the dedicated Release cache")
        [void](Invoke-ExternalStep "configure" $cmake @("-S", $RepoRoot, "-B", $BuildDir, "-G", $generator, "-A", "x64"))
        [void](Assert-SafeRepositoryDirectory -RepositoryRoot $RepoRoot -TargetPath $BuildDir -Description "use the configured Release cache")
        [void](Assert-SafeBuildTree -BuildPath $BuildDir)
        if (!(Test-Path -LiteralPath $cacheFile)) { throw "CMake configure succeeded without creating CMakeCache.txt." }
        [void](Assert-CompatibleCache -CacheFile $cacheFile -ExpectedSource $RepoRoot -ExpectedBuild $BuildDir -AdvertisedGenerators $script:AdvertisedGenerators)
    }

    [void](Assert-SafeRepositoryDirectory -RepositoryRoot $RepoRoot -TargetPath $BuildDir -Description "build the Release cache")
    [void](Assert-SafeBuildTree -BuildPath $BuildDir)
    [void](Invoke-ExternalStep "build-release" $cmake @("--build", $BuildDir, "--config", "Release"))
    [void](Assert-SafeBuildTree -BuildPath $BuildDir)
    $binary = Join-Path $BuildDir "Release\ClassicNotepad.exe"
    if (!(Test-Path -LiteralPath $binary)) { throw "Release executable was not produced: $binary" }
    $script:BinaryHash = Get-Sha256Bytes ([System.IO.File]::ReadAllBytes($binary))

    [void](Invoke-ExternalStep "ctest-release" $ctest @("--test-dir", $BuildDir, "-C", "Release", "--output-on-failure"))
    $script:AutomationResultPath = Join-Path $EvidenceDir "$($script:RunTag)-automation-$($Suite.ToLowerInvariant()).json"
    [void](Assert-SafeRepositoryDirectory -RepositoryRoot $RepoRoot -TargetPath $EvidenceDir -Description "write structured automation results")
    $script:AutomationAttempted = $true
    try {
        $automationStep = Invoke-ExternalStep "automation-$Suite" $python @(
            $Runner,
            "--binary", $binary,
            "--platform", "windows",
            "--suite", $Suite,
            "--json-results", $script:AutomationResultPath
        )
        $script:AutomationChildExitCode = $automationStep.exitCode
    } catch {
        if ($_.Exception.Data.Contains("ChildExitCode")) {
            $script:AutomationChildExitCode = [int]$_.Exception.Data["ChildExitCode"]
        }
        throw
    }
    Write-Host "Release verification succeeded."
} catch {
    $script:FailureMessage = $_.Exception.Message
    if ($_.Exception.Data.Contains("ChildExitCode")) {
        $script:ExitCode = [int]$_.Exception.Data["ChildExitCode"]
    } else {
        $script:ExitCode = 1
    }
    Write-Error -Message $script:FailureMessage -ErrorAction Continue
} finally {
    $versionHashAfter = $null
    $versionRestorationError = $null
    try {
        if ($versionExisted -and $null -ne $versionSnapshot) {
            $currentExists = Test-Path -LiteralPath $VersionFile
            $currentHash = if ($currentExists) { Get-Sha256Bytes ([System.IO.File]::ReadAllBytes($VersionFile)) } else { $null }
            if ($currentHash -ne $versionHashBefore) {
                $script:VersionUnexpectedMutation = $true
                [System.IO.File]::WriteAllBytes($VersionFile, $versionSnapshot)
                $script:VersionRestored = $true
            }
            $versionHashAfter = Get-Sha256Bytes ([System.IO.File]::ReadAllBytes($VersionFile))
            if ($versionHashAfter -ne $versionHashBefore) {
                throw "VERSION hash differs after restoration (expected $versionHashBefore, got $versionHashAfter)."
            }
            if ($script:VersionUnexpectedMutation -and $script:ExitCode -eq 0) {
                $script:ExitCode = 1
                $script:FailureMessage = "A successful child process unexpectedly changed VERSION; original bytes were restored."
            }
        } elseif (!$versionExisted -and (Test-Path -LiteralPath $VersionFile)) {
            Remove-Item -LiteralPath $VersionFile -Force
            $script:VersionRestored = $true
        }
    } catch {
        $versionRestorationError = $_.Exception.Message
        Write-Error -Message "VERSION preservation failed: $versionRestorationError" -ErrorAction Continue
        if ($script:ExitCode -eq 0) { $script:ExitCode = 1 }
    }

    if ($script:AutomationAttempted) { Validate-AutomationResult }

    try {
        if (!$script:EvidenceDirectorySafe) {
            throw "Evidence directory failed containment validation; no evidence was written outside the repository."
        }
        [void](Assert-SafeRepositoryDirectory -RepositoryRoot $RepoRoot -TargetPath $EvidenceDir -Description "write the gate result")
        if (!(Test-Path -LiteralPath $EvidenceDir)) { [void](New-Item -ItemType Directory -Path $EvidenceDir -Force) }
        [void](Assert-SafeRepositoryDirectory -RepositoryRoot $RepoRoot -TargetPath $EvidenceDir -Description "write the gate result")
        $sourceIdentity = Get-SourceIdentity
        $record = [ordered]@{
            schemaVersion = 1
            startedUtc = $startedAt.ToString("o")
            completedUtc = [DateTime]::UtcNow.ToString("o")
            status = if ($script:ExitCode -eq 0) { "passed" } else { "failed" }
            exitCode = $script:ExitCode
            failure = $script:FailureMessage
            suite = $Suite
            repository = $RepoRoot
            sourceRevision = $sourceIdentity.revision
            sourceDirty = $sourceIdentity.dirty
            operatingSystem = [Environment]::OSVersion.VersionString
            architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
            dedicatedBuildDirectory = $BuildDir
            generator = $script:SelectedGenerator
            manifestPath = $script:ManifestPath
            manifestSha256 = $script:ManifestSha256
            toolVersions = $script:ToolInfo
            binary = if ($script:BinaryHash) { Join-Path $BuildDir "Release\ClassicNotepad.exe" } else { $null }
            binarySha256 = $script:BinaryHash
            versionSha256Before = $versionHashBefore
            versionSha256After = $versionHashAfter
            versionRestored = $script:VersionRestored
            versionUnexpectedMutation = $script:VersionUnexpectedMutation
            versionRestorationError = $versionRestorationError
            automationResultsJson = $script:AutomationResultPath
            automationResultSha256 = if ($script:AutomationResultPath -and (Test-Path -LiteralPath $script:AutomationResultPath -PathType Leaf)) { Get-Sha256Bytes ([System.IO.File]::ReadAllBytes($script:AutomationResultPath)) } else { $null }
            automationValidation = $script:AutomationValidation
            steps = @($script:StepResults)
        }
        $evidenceName = "windows-release-$($Suite.ToLowerInvariant())-$($startedAt.ToString('yyyyMMddTHHmmssZ'))-$PID.json"
        $evidencePath = Join-Path $EvidenceDir $evidenceName
        $json = ConvertTo-Json -InputObject $record -Depth 8
        [System.IO.File]::WriteAllText($evidencePath, $json + [Environment]::NewLine, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "Result record: $evidencePath"
    } catch {
        Write-Error -Message "Could not write gate result record: $($_.Exception.Message)" -ErrorAction Continue
        if ($script:ExitCode -eq 0) { $script:ExitCode = 1 }
    }
}

exit $script:ExitCode
