[CmdletBinding()]
param([string]$RunId, [ValidateRange(1, 32)][int]$Parallelism = 4)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$base = Join-Path $repo 'build\windows-qualification'
$scratch = Join-Path $repo 'build\automation-tests'
$runner = Join-Path $repo 'tests\automation\run_automation_tests.py'
$manifest = Join-Path $repo 'tests\automation\test_suite_manifest.json'
$versionFile = Join-Path $repo 'VERSION'
$e10FullPath = Join-Path $repo 'build\windows-release-verification-evidence\20261004T233852Z-round2-full.json'
$e10SummaryPath = Join-Path $repo 'build\windows-release-verification-evidence\20261004T233852Z-round2-summary.json'
$e10GatePath = Join-Path $repo 'build\windows-release-verification-evidence\windows-release-ordinary-20261004T233639Z-9236.json'
$script:commands = [Collections.Generic.List[object]]::new()
$script:failed = $null
$script:firstNonzeroChildExitCode = $null
$script:sourceDrift = $false
$script:sourceBefore = $null
$script:sourceAfter = $null
$script:scratchBefore = $null
$script:scratchAfter = $null
$script:versionBefore = $null
$script:versionAfter = $null
$script:binary = $null
$script:binaryHash = $null
$script:stepNumber = 0
$script:sourceDirty = $null
$script:fullReuse = $null
$script:pendingReasons = [Collections.Generic.List[string]]::new()
$script:environmentUnverifiedItems = @(
    'Windows power plan state',
    'Per-display DPI and scaling state',
    'Font, application theme, and high-contrast state',
    'Spelling provider and dictionary availability/selection',
    'WinSta0/window-station/desktop isolation capability and clipboard isolation capability',
    'Storage type (local SSD/HDD, network, or virtualized)',
    'ANSI code page'
)

function Hash-File([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToUpperInvariant()
}
function File-Reference([AllowNull()][string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return [ordered]@{ path = $null; exists = $false; sha256 = $null; error = $null }
    }
    $exists = $false
    $hash = $null
    $referenceError = $null
    try {
        $exists = Test-Path -LiteralPath $Path -PathType Leaf -ErrorAction Stop
        if ($exists) { $hash = Hash-File $Path }
    } catch { $referenceError = $_.Exception.Message }
    [ordered]@{
        path = $Path
        exists = [bool]$exists
        sha256 = $hash
        error = $referenceError
    }
}
function Assert-NoReparsePathChain([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $pathRoot = [IO.Path]::GetPathRoot($fullPath)
    $current = $pathRoot
    $remainder = $fullPath.Substring($pathRoot.Length)
    foreach ($part in ($remainder -split '[\\/]')) {
        if ([string]::IsNullOrWhiteSpace($part)) { continue }
        $current = Join-Path $current $part
        $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing path because a component is a reparse point: $current"
        }
    }
}
function Assert-RunPathSafe([string]$Path, [switch]$MustNotExist, [switch]$MustExist) {
    $root = [IO.Path]::GetFullPath($repo).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $prefix = $root + [IO.Path]::DirectorySeparatorChar
    if (!$fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing output outside repository root: $fullPath"
    }
    Assert-NoReparsePathChain $root
    Assert-NoReparsePathChain $fullPath
    if ($MustNotExist -and (Test-Path -LiteralPath $fullPath)) {
        throw "Refusing to write to an existing path: $fullPath"
    }
    if ($MustExist -and !(Test-Path -LiteralPath $fullPath)) {
        throw "Expected guarded path to exist: $fullPath"
    }

    $resolvedRoot = (Resolve-Path -LiteralPath $root -ErrorAction Stop).ProviderPath.TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $existing = $fullPath
    while (!(Test-Path -LiteralPath $existing)) {
        $parent = Split-Path -Parent $existing
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $existing) {
            throw "No existing parent found for guarded path: $fullPath"
        }
        $existing = $parent
    }
    $resolvedExisting = (Resolve-Path -LiteralPath $existing -ErrorAction Stop).ProviderPath.TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $resolvedPrefix = $resolvedRoot + [IO.Path]::DirectorySeparatorChar
    if ($resolvedExisting -ne $resolvedRoot -and
        !$resolvedExisting.StartsWith($resolvedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing output because its nearest existing parent resolves outside repository: $resolvedExisting"
    }
}
function Save-Json([string]$Path, [object]$Value) {
    Assert-RunPathSafe $Path -MustNotExist
    [IO.File]::WriteAllText($Path, (ConvertTo-Json -InputObject $Value -Depth 60) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
}
function Assert-NewRun([string]$Path) {
    Assert-RunPathSafe $Path -MustNotExist
}
function Invoke-Logged([string]$Name, [string]$Exe, [string[]]$Arguments) {
    $script:stepNumber++
    $stem = '{0:D2}-{1}' -f $script:stepNumber, ($Name -replace '[^A-Za-z0-9_.-]', '_')
    $out = Join-Path (Join-Path $script:run 'logs') "$stem.stdout.log"
    $err = Join-Path (Join-Path $script:run 'logs') "$stem.stderr.log"
    Assert-RunPathSafe $out -MustNotExist
    Assert-RunPathSafe $err -MustNotExist
    $started = [DateTime]::UtcNow
    $exit = 1
    $launchError = $null
    $oldPreference = $ErrorActionPreference
    try {
        if (!(Test-Path -LiteralPath $Exe -PathType Leaf)) { throw "Executable not found: $Exe" }
        $ErrorActionPreference = 'Continue'
        & $Exe @Arguments 1> $out 2> $err
        $exit = $LASTEXITCODE
        if ($exit -ne 0 -and $null -eq $script:firstNonzeroChildExitCode) {
            $script:firstNonzeroChildExitCode = [int]$exit
        }
    } catch {
        $launchError = $_.Exception.Message
        [IO.File]::WriteAllText($err, $launchError, [Text.UTF8Encoding]::new($false))
        if (!(Test-Path $out)) { [IO.File]::WriteAllText($out, '', [Text.UTF8Encoding]::new($false)) }
    } finally { $ErrorActionPreference = $oldPreference }
    $record = [ordered]@{
        name = $Name
        executable = $Exe
        arguments = @($Arguments)
        startedUtc = $started.ToString('o')
        finishedUtc = [DateTime]::UtcNow.ToString('o')
        exitCode = $exit
        launchError = $launchError
        stdoutLog = $out
        stderrLog = $err
    }
    $script:commands.Add($record)
    $stdout = if (Test-Path $out) { [IO.File]::ReadAllText($out) } else { '' }
    Write-Host "$Name exit=$exit (stdout=$out; stderr=$err)"
    [pscustomobject]@{ record = $record; stdout = $stdout }
}
function Get-SourceHashSet {
    $files = [Collections.Generic.List[object]]::new()
    $extensions = @('.c','.cc','.cpp','.h','.hh','.hpp','.py','.ps1','.json','.md','.txt','.cmake','.rc','.xml','.yml','.yaml','.sh','.bat','.vcxproj','.props','.targets','.manifest','.png','.ico','.in')
    # Docs are written after the qualified command sequence. The source identity
    # covers production, tests, scripts, assets, and build inputs used by it.
    foreach ($dir in @('src','tests','scripts','assets','clasicNotepadCrossPlatform')) {
        $root = Join-Path $repo $dir
        if (!(Test-Path $root -PathType Container)) { continue }
        foreach ($item in Get-ChildItem -LiteralPath $root -File -Recurse -Force) {
            if ($item.FullName -match '[\\/](build[^\\/]*|__pycache__|\.git)[\\/]') { continue }
            if ($item.Extension.ToLowerInvariant() -in $extensions) {
                $files.Add([pscustomobject]@{ path = $item.FullName.Substring($repo.Length).TrimStart('\'); sha256 = Hash-File $item.FullName })
            }
        }
    }
    foreach ($name in @('CMakeLists.txt','CMakePresets.json')) {
        $p = Join-Path $repo $name
        if (Test-Path $p -PathType Leaf) { $files.Add([pscustomobject]@{ path = $name; sha256 = Hash-File $p }) }
    }
    $ordered = @($files | Sort-Object path -Unique)
    $text = ($ordered | ForEach-Object { $_.path + ' ' + $_.sha256 }) -join [Environment]::NewLine
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { $digestBytes = $algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)) }
    finally { $algorithm.Dispose() }
    $digest = [BitConverter]::ToString($digestBytes).Replace('-', '')
    [ordered]@{ capturedUtc = [DateTime]::UtcNow.ToString('o'); aggregateSha256 = $digest; fileCount = $ordered.Count; files = $ordered }
}
function Get-Scratch {
    $items = [Collections.Generic.List[object]]::new()
    if (Test-Path $scratch -PathType Container) {
        # Inventory only immediate children. Several pre-existing child trees
        # are ACL-opaque; never recurse into or change them.
        foreach ($item in Get-ChildItem -LiteralPath $scratch -Force) {
            $row = [ordered]@{
                path = $item.Name
                directory = $item.PSIsContainer
                length = if ($item.PSIsContainer) { $null } else { $item.Length }
                sha256 = $null
                attributes = $item.Attributes.ToString()
                creationTimeUtcTicks = $item.CreationTimeUtc.Ticks
                lastWriteTimeUtcTicks = $item.LastWriteTimeUtc.Ticks
                inventoryDepth = 0
                directoryContentsHashed = $false
            }
            if (!$item.PSIsContainer) { $row.length = $item.Length; $row.sha256 = Hash-File $item.FullName }
            $items.Add($row)
        }
    }
    [ordered]@{ exists = (Test-Path $scratch); count = $items.Count; entries = @($items | Sort-Object path) }
}
function Compare-Scratch([object]$Before, [object]$After) {
    $beforeByName = @{}
    $afterByName = @{}
    foreach ($entry in @($Before.entries)) { $beforeByName[[string]$entry.path] = $entry }
    foreach ($entry in @($After.entries)) { $afterByName[[string]$entry.path] = $entry }
    $removed = @($beforeByName.Keys | Where-Object { !$afterByName.ContainsKey($_) } | Sort-Object)
    $added = @($afterByName.Keys | Where-Object { !$beforeByName.ContainsKey($_) } | Sort-Object)
    $changed = [Collections.Generic.List[object]]::new()
    foreach ($name in @($beforeByName.Keys | Where-Object { $afterByName.ContainsKey($_) } | Sort-Object)) {
        $a = $beforeByName[$name]
        $b = $afterByName[$name]
        $same = $a.directory -eq $b.directory -and $a.length -eq $b.length -and
            $a.sha256 -eq $b.sha256 -and $a.attributes -eq $b.attributes -and
            $a.creationTimeUtcTicks -eq $b.creationTimeUtcTicks -and
            $a.lastWriteTimeUtcTicks -eq $b.lastWriteTimeUtcTicks
        if (!$same) { $changed.Add([pscustomobject]@{ path = $name; before = $a; after = $b }) }
    }
    [ordered]@{ removed = $removed; added = $added; changed = @($changed) }
}
function Query([scriptblock]$Block) {
    try { [ordered]@{ ok = $true; data = @(& $Block); error = $null } }
    catch { [ordered]@{ ok = $false; data = @(); error = $_.Exception.Message } }
}
function Get-Inventory {
    $screens = @()
    $displayNote = $null
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        $screens = @([Windows.Forms.Screen]::AllScreens | ForEach-Object { [ordered]@{ device = $_.DeviceName; bounds = $_.Bounds.ToString(); workArea = $_.WorkingArea.ToString(); primary = $_.Primary } })
    } catch { $displayNote = $_.Exception.Message }
    $languages = @()
    try { $languages = @(Get-WinUserLanguageList | ForEach-Object { [ordered]@{ tag = $_.LanguageTag; inputMethods = @($_.InputMethodTips) } }) } catch {}
    $sysLocale = $null
    try { $sysLocale = (Get-WinSystemLocale).Name } catch {}
    [ordered]@{
        capturedUtc = [DateTime]::UtcNow.ToString('o')
        machineName = $env:COMPUTERNAME
        os = Query { Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,OSArchitecture,Locale }
        hardware = Query { Get-CimInstance Win32_ComputerSystem | Select-Object Manufacturer,Model,SystemType,TotalPhysicalMemory,NumberOfLogicalProcessors }
        processors = Query { Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed }
        systemDrive = Query { Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$($env:SystemDrive)'" | Select-Object DeviceID,FileSystem,Size,FreeSpace,VolumeName }
        keyboards = Query { Get-CimInstance Win32_Keyboard | Select-Object Name,Description,Layout }
        pointingDevices = Query { Get-CimInstance Win32_PointingDevice | Select-Object Name,Description,Manufacturer }
        videoControllers = Query { Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,VideoModeDescription,CurrentHorizontalResolution,CurrentVerticalResolution }
        printers = Query { if (Get-Command Get-Printer -ErrorAction SilentlyContinue) { Get-Printer | Select-Object Name,DriverName,PortName,PrinterStatus } else { Get-CimInstance Win32_Printer | Select-Object Name,DriverName,PortName,PrinterStatus } }
        printerPorts = Query { if (Get-Command Get-PrinterPort -ErrorAction SilentlyContinue) { Get-PrinterPort | Select-Object Name,Description,PrinterHostAddress,PortNumber } else { Get-CimInstance Win32_TCPIPPrinterPort | Select-Object Name,HostAddress,PortNumber,Protocol } }
        culture = [Globalization.CultureInfo]::CurrentCulture.Name
        systemLocale = $sysLocale
        userLanguages = $languages
        screens = $screens
        displayNote = $displayNote
        session = [ordered]@{ id = [Diagnostics.Process]::GetCurrentProcess().SessionId; name = $env:SESSIONNAME; user = $env:USERNAME; userInteractive = [Environment]::UserInteractive }
        clipboard = [ordered]@{ accessed = $false; isolatedCapability = 'not probed; no clipboard was opened, read, or written' }
        explicitlyNotCaptured = $script:environmentUnverifiedItems
        powershell = $PSVersionTable.PSVersion.ToString()
        architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    }
}
function Assert-SourceStable([string]$Stage) {
    $now = Get-SourceHashSet
    if ($now.aggregateSha256 -ne $script:sourceBefore.aggregateSha256) {
        $script:sourceAfter = $now
        $script:sourceDrift = $true
        throw "Source identity changed during $Stage; stopped and retained evidence."
    }
}

if ([string]::IsNullOrWhiteSpace($RunId)) { $RunId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + "-$PID-" + [Guid]::NewGuid().ToString('N').Substring(0,8) }
if ($RunId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,80}$') { throw 'Invalid run ID.' }
$baseFull = [IO.Path]::GetFullPath($base)
Assert-RunPathSafe $baseFull
if (Test-Path -LiteralPath $baseFull -PathType Leaf) { throw "Qualification output base is not a directory: $baseFull" }
if (!(Test-Path -LiteralPath $baseFull)) { New-Item -ItemType Directory -Path $baseFull | Out-Null }
Assert-RunPathSafe $baseFull -MustExist
$script:run = Join-Path $base $RunId
Assert-NewRun $script:run
try { New-Item -ItemType Directory -Path $script:run -ErrorAction Stop | Out-Null }
catch { throw "Could not atomically create a new qualification run directory: $script:run; $($_.Exception.Message)" }
Assert-RunPathSafe $script:run -MustExist
$logs = Join-Path $script:run 'logs'
Assert-RunPathSafe $logs -MustNotExist
New-Item -ItemType Directory -Path $logs | Out-Null
Assert-RunPathSafe $logs -MustExist
$build = Join-Path $script:run 'cmake-build'
$recordPath = Join-Path $script:run 'baseline.json'
$started = [DateTime]::UtcNow
$summaryCtest = $null
$summaryFull = $null
$baseRevision = $null

try {
    $inventory = Get-Inventory
    Save-Json (Join-Path $script:run 'environment-before.json') $inventory
    $script:scratchBefore = Get-Scratch
    Save-Json (Join-Path $script:run 'automation-scratch-before.json') $script:scratchBefore
    $script:sourceBefore = Get-SourceHashSet
    Save-Json (Join-Path $script:run 'source-before.json') $script:sourceBefore
    $script:versionBefore = Hash-File $versionFile

    $git = (Get-Command git -ErrorAction Stop).Source
    $rev = Invoke-Logged 'git-revision' $git @('rev-parse','HEAD')
    if ($rev.record.exitCode -ne 0) { throw 'git rev-parse failed.' }
    $baseRevision = $rev.stdout.Trim()
    $gitState = Invoke-Logged 'git-working-tree-state' $git @('status','--porcelain=v2','--untracked-files=all')
    if ($gitState.record.exitCode -ne 0) { throw 'git status failed.' }
    $script:sourceDirty = ![string]::IsNullOrWhiteSpace($gitState.stdout)
    $vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path $vswhere -PathType Leaf)) { throw "Missing required tool: $vswhere" }
    $vs = Invoke-Logged 'vswhere-vs18-cxx' $vswhere @('-all','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-format','json')
    if ($vs.record.exitCode -ne 0) { throw 'vswhere inventory failed.' }
    # Assign the root JSON array directly. Pipeline input keeps it nested on
    # Windows PowerShell 5.1, which would join every installation path later.
    $instances = ConvertFrom-Json -InputObject $vs.stdout
    $instanceInfo = @($instances | Where-Object { $_.installationVersion -match '^18\.' -and $_.installationPath -eq 'C:\Program Files\Microsoft Visual Studio\18\Community' -and $_.isComplete -and $_.isLaunchable })
    if ($instanceInfo.Count -ne 1) { throw 'Expected a unique installed VS18 Community C++ instance.' }
    $instance = $instanceInfo[0].installationPath
    $cmake = Join-Path $instance 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $ctest = Join-Path $instance 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
    $python = 'C:\Python313\python.exe'
    foreach ($tool in @($cmake,$ctest,$python)) { if (!(Test-Path $tool -PathType Leaf)) { throw "Missing required tool: $tool" } }
    $vsv = Invoke-Logged 'vswhere-version' $vswhere @('-?')
    if ($vsv.record.exitCode -ne 0) { throw 'vswhere version probe failed.' }
    $cmv = Invoke-Logged 'cmake-version' $cmake @('--version')
    $ctv = Invoke-Logged 'ctest-version' $ctest @('--version')
    $cmh = Invoke-Logged 'cmake-generators' $cmake @('--help')
    $pyv = Invoke-Logged 'python-version' $python @('--version')
    if ($cmh.record.exitCode -ne 0 -or $cmh.stdout -notmatch 'Visual Studio 18 2026') { throw 'CMake does not advertise the installed VS18 generator.' }
    foreach ($probe in @($cmv,$ctv,$pyv)) { if ($probe.record.exitCode -ne 0) { throw "$($probe.record.name) failed." } }
    $toolInfo = [ordered]@{
        visualStudio = $instanceInfo[0]
        generator = 'Visual Studio 18 2026'
        cmake = $cmake
        cmakeVersion = ($cmv.stdout -split '\r?\n')[0]
        ctest = $ctest
        ctestVersion = ($ctv.stdout -split '\r?\n')[0]
        python = $python
        pythonVersion = $pyv.stdout.Trim()
        vswhere = $vswhere
        vswhereSha256 = Hash-File $vswhere
        cmakeSha256 = Hash-File $cmake
        ctestSha256 = Hash-File $ctest
        pythonSha256 = Hash-File $python
        vswhereVersion = ($vsv.stdout -split '\r?\n' | Select-Object -First 1).Trim()
        runnerSha256 = Hash-File $runner
        manifestSha256 = Hash-File $manifest
    }
    Save-Json (Join-Path $script:run 'tools.json') $toolInfo
    Assert-SourceStable 'tool preflight'

    Assert-RunPathSafe $build -MustNotExist
    $configure = Invoke-Logged 'cmake-configure-release-x64' $cmake @('-S',$repo,'-B',$build,'-G','Visual Studio 18 2026','-A','x64',"-DCMAKE_GENERATOR_INSTANCE=$instance")
    if ($configure.record.exitCode -ne 0) { throw "CMake configure failed: $($configure.record.exitCode)" }
    Assert-RunPathSafe $build -MustExist
    Assert-SourceStable 'configure'
    $built = Invoke-Logged 'cmake-build-release-x64' $cmake @('--build',$build,'--config','Release','--parallel',[string]$Parallelism)
    if ($built.record.exitCode -ne 0) { throw "Release build failed: $($built.record.exitCode)" }
    Assert-SourceStable 'Release build'

    $script:binary = Join-Path $build 'Release\ClassicNotepad.exe'
    if (!(Test-Path $script:binary -PathType Leaf)) { throw "Release executable missing: $script:binary" }
    $script:binaryHash = Hash-File $script:binary
    $ctList = Invoke-Logged 'ctest-list-release' $ctest @('--test-dir',$build,'-C','Release','--show-only=json-v1')
    if ($ctList.record.exitCode -ne 0) { throw "CTest discovery failed: $($ctList.record.exitCode)" }
    $ctListJson = $ctList.stdout | ConvertFrom-Json
    $ctestRegisteredIds = @($ctListJson.tests | ForEach-Object { [string]$_.name })
    if (!$ctestRegisteredIds.Count -or (@($ctestRegisteredIds | Sort-Object -Unique).Count -ne $ctestRegisteredIds.Count)) { throw 'CTest registered test IDs are empty or duplicated.' }
    $ct = Invoke-Logged 'ctest-release' $ctest @('--test-dir',$build,'-C','Release','--output-on-failure')
    if ($ct.record.exitCode -ne 0) { throw "CTest failed: $($ct.record.exitCode)" }
    $ctestExecutedIds = @([regex]::Matches($ct.stdout, '(?m)^\s*\d+/\d+ Test\s+#\d+:\s+(.+?)\s+\.{3,}\s+Passed(?:\s+\d+(?:\.\d+)?\s+sec)?\s*$') | ForEach-Object { $_.Groups[1].Value.Trim() })
    if ($ctestExecutedIds.Count -ne $ctestRegisteredIds.Count -or
        (($ctestExecutedIds | Sort-Object) -join '|') -ne (($ctestRegisteredIds | Sort-Object) -join '|')) {
        throw 'CTest registered IDs and passing executed IDs do not reconcile.'
    }
    if ($ct.stdout -match '(?m)^\s*(\d+)% tests passed,\s*(\d+) tests failed out of (\d+)') {
        $summaryCtest = [ordered]@{ passed = [int]$Matches[3]-[int]$Matches[2]; failed = [int]$Matches[2]; total = [int]$Matches[3]; percent = [int]$Matches[1] }
    } else { throw 'Could not read CTest totals from output.' }
    $summaryCtest.registeredIds = $ctestRegisteredIds
    $summaryCtest.executedIds = $ctestExecutedIds
    $summaryCtest.idsReconcile = $true
    if ($summaryCtest.total -ne $ctestRegisteredIds.Count -or $summaryCtest.failed -ne 0) { throw 'CTest totals do not match its registered test IDs.' }
    Assert-SourceStable 'CTest'

    # Full automation includes real Windows Ctrl+C/Ctrl+V cases. Reuse the
    # retained E10 Full record only after identity checks; never launch it here.
    $identity = [ordered]@{
        e10FullRecordExists = (Test-Path -LiteralPath $e10FullPath -PathType Leaf)
        e10SummaryExists = (Test-Path -LiteralPath $e10SummaryPath -PathType Leaf)
        e10GateExists = (Test-Path -LiteralPath $e10GatePath -PathType Leaf)
        binarySha256Matches = $false
        runnerSha256Matches = $false
        manifestSha256Matches = $false
        operatingSystemMatches = $false
        architectureMatches = $false
        vswhereVersionMatches = $false
        generatorMatches = $false
        visualStudioVersionMatches = $false
        cmakeVersionMatches = $false
        ctestVersionMatches = $false
        pythonVersionMatches = $false
        sourceRevisionMatches = $false
        sourceDirtyFlagMatches = $false
        versionSha256BeforeMatches = $false
        versionSha256AfterMatches = $false
        fullLogsPresent = $false
        historicalFullRecordValid = $false
    }
    $e10 = $null
    $e10Summary = $null
    $e10Gate = $null
    if ($identity.e10FullRecordExists -and $identity.e10SummaryExists -and $identity.e10GateExists) {
        $e10 = Get-Content -LiteralPath $e10FullPath -Raw | ConvertFrom-Json
        $e10Summary = Get-Content -LiteralPath $e10SummaryPath -Raw | ConvertFrom-Json
        $e10Gate = Get-Content -LiteralPath $e10GatePath -Raw | ConvertFrom-Json
        $e10Ids = @($e10.selectedIds)
        $e10Executed = @($e10.executedIds)
        $e10OutcomeIds = @($e10.outcomes | ForEach-Object { [string]$_.testId })
        $e10StatusGroups = [ordered]@{
            passed = @($e10.outcomes | Where-Object { $_.status -eq 'passed' }).Count
            failed = @($e10.outcomes | Where-Object { $_.status -eq 'failed' }).Count
            errors = @($e10.outcomes | Where-Object { $_.status -eq 'error' }).Count
            skipped = @($e10.outcomes | Where-Object { $_.status -eq 'skipped' }).Count
            expectedFailures = @($e10.outcomes | Where-Object { $_.status -eq 'expectedFailure' }).Count
            unexpectedSuccesses = @($e10.outcomes | Where-Object { $_.status -eq 'unexpectedSuccess' }).Count
        }
        $e10KnownStatusCount = @($e10.outcomes | Where-Object { $_.status -in @('passed','failed','error','skipped','expectedFailure','unexpectedSuccess') }).Count
        $e10CountsSum = [int]$e10.counts.passed + [int]$e10.counts.failed + [int]$e10.counts.errors + [int]$e10.counts.skipped + [int]$e10.counts.expectedFailures + [int]$e10.counts.unexpectedSuccesses
        $e10IdsReconcile = $e10Ids.Count -eq $e10Executed.Count -and $e10Ids.Count -eq $e10OutcomeIds.Count -and
            (@($e10Ids | Sort-Object -Unique).Count -eq $e10Ids.Count) -and
            (($e10Ids | Sort-Object) -join '|') -eq (($e10Executed | Sort-Object) -join '|') -and
            (($e10Ids | Sort-Object) -join '|') -eq (($e10OutcomeIds | Sort-Object) -join '|')
        $e10CountsReconcile = $e10Ids.Count -eq [int]$e10.counts.selected -and
            $e10OutcomeIds.Count -eq [int]$e10.counts.executed -and $e10CountsSum -eq $e10OutcomeIds.Count -and
            $e10KnownStatusCount -eq $e10OutcomeIds.Count -and
            $e10StatusGroups.passed -eq [int]$e10.counts.passed -and
            $e10StatusGroups.failed -eq [int]$e10.counts.failed -and
            $e10StatusGroups.errors -eq [int]$e10.counts.errors -and
            $e10StatusGroups.skipped -eq [int]$e10.counts.skipped -and
            $e10StatusGroups.expectedFailures -eq [int]$e10.counts.expectedFailures -and
            $e10StatusGroups.unexpectedSuccesses -eq [int]$e10.counts.unexpectedSuccesses
        $fullSuiteEvidence = @($e10Summary.additionalSuites | Where-Object { $_.suite -eq 'Full' -and $_.resultJson -eq $e10FullPath }) | Select-Object -First 1
        $identity.binarySha256Matches = $e10.binarySha256 -eq $script:binaryHash -and $e10Summary.binarySha256 -eq $script:binaryHash
        $identity.runnerSha256Matches = $e10.runnerPath -eq $runner -and $e10Summary.runner -eq $runner -and
            $e10.runnerSha256 -eq $toolInfo.runnerSha256 -and $e10Summary.runnerSha256 -eq $toolInfo.runnerSha256
        $identity.manifestSha256Matches = $e10.manifestPath -eq $manifest -and $e10Summary.manifest -eq $manifest -and
            $e10.manifestSha256 -eq $toolInfo.manifestSha256 -and $e10Summary.manifestSha256 -eq $toolInfo.manifestSha256
        $identity.operatingSystemMatches = $e10Gate.operatingSystem -eq [Environment]::OSVersion.ToString()
        $identity.architectureMatches = $e10Gate.architecture -eq [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
        $identity.vswhereVersionMatches = $e10Gate.toolVersions.vswhere.version -eq $toolInfo.vswhereVersion
        $identity.generatorMatches = $e10Summary.generator -eq $toolInfo.generator -and $e10Gate.generator -eq $toolInfo.generator
        $e10Vs = @($e10Gate.toolVersions.visualStudioInstances | Where-Object { $_.installationPath -eq $instance }) | Select-Object -First 1
        $identity.visualStudioVersionMatches = $null -ne $e10Vs -and $e10Vs.installationVersion -eq $toolInfo.visualStudio.installationVersion
        $identity.cmakeVersionMatches = $e10Gate.toolVersions.cmake.path -eq $cmake -and $e10Gate.toolVersions.cmake.version -eq $toolInfo.cmakeVersion
        $identity.ctestVersionMatches = $e10Gate.toolVersions.ctest.path -eq $ctest -and $e10Gate.toolVersions.ctest.version -eq $toolInfo.ctestVersion
        $identity.pythonVersionMatches = $e10Gate.toolVersions.python.path -eq $python -and $e10Gate.toolVersions.python.version -eq $toolInfo.pythonVersion
        $identity.sourceRevisionMatches = $e10Summary.sourceRevision -eq $baseRevision -and $e10Gate.sourceRevision -eq $baseRevision
        $identity.sourceDirtyFlagMatches = [bool]$e10Summary.sourceDirty -eq [bool]$script:sourceDirty -and [bool]$e10Gate.sourceDirty -eq [bool]$script:sourceDirty
        $identity.versionSha256BeforeMatches = $e10Gate.versionSha256Before -eq $script:versionBefore
        $identity.versionSha256AfterMatches = $e10Gate.versionSha256After -eq $script:versionBefore
        $identity.fullLogsPresent = $null -ne $fullSuiteEvidence -and
            (Test-Path -LiteralPath $fullSuiteEvidence.stdoutLog -PathType Leaf) -and
            (Test-Path -LiteralPath $fullSuiteEvidence.stderrLog -PathType Leaf)
        $summaryCountsMatch = $null -ne $fullSuiteEvidence -and
            [int]$fullSuiteEvidence.counts.selected -eq [int]$e10.counts.selected -and
            [int]$fullSuiteEvidence.counts.executed -eq [int]$e10.counts.executed -and
            [int]$fullSuiteEvidence.counts.passed -eq [int]$e10StatusGroups.passed -and
            [int]$fullSuiteEvidence.counts.failed -eq [int]$e10StatusGroups.failed -and
            [int]$fullSuiteEvidence.counts.errors -eq [int]$e10StatusGroups.errors -and
            [int]$fullSuiteEvidence.counts.skipped -eq [int]$e10StatusGroups.skipped -and
            [int]$fullSuiteEvidence.counts.expectedFailures -eq [int]$e10StatusGroups.expectedFailures -and
            [int]$fullSuiteEvidence.counts.unexpectedSuccesses -eq [int]$e10StatusGroups.unexpectedSuccesses
        $identity.historicalFullRecordValid = $e10.suite -eq 'Full' -and [int]$e10.exitCode -eq 0 -and
            $e10Summary.status -eq 'passed' -and $e10Gate.status -eq 'passed' -and
            $e10IdsReconcile -and $e10CountsReconcile -and $summaryCountsMatch -and $identity.fullLogsPresent -and
            $e10.resultPath -eq $e10FullPath -and
            [int]$fullSuiteEvidence.exitCode -eq 0 -and [bool]$fullSuiteEvidence.reconciled -and
            [int]$e10.counts.failed -eq 0 -and [int]$e10.counts.errors -eq 0 -and
            [int]$e10.counts.unexpectedSuccesses -eq 0
    }
    $failedIdentityChecks = @($identity.GetEnumerator() | Where-Object { $_.Key -like '*Exists' -or $_.Key -like '*Matches' -or $_.Key -eq 'historicalFullRecordValid' } | Where-Object { !$_.Value } | ForEach-Object { $_.Key })
    $reuseOkay = $failedIdentityChecks.Count -eq 0
    if (!$reuseOkay) { foreach ($name in $failedIdentityChecks) { $script:pendingReasons.Add("Historical Full reuse check failed: $name") } }
    $script:fullReuse = [ordered]@{
        status = if ($reuseOkay) { 'reused_historical_same_binary' } else { 'pending_identity_mismatch' }
        rerun = $false
        identityChecks = $identity
        pendingReasons = @($script:pendingReasons)
        historicalFullJson = $e10FullPath
        historicalFullJsonSha256 = if ($identity.e10FullRecordExists) { Hash-File $e10FullPath } else { $null }
        historicalSummaryJson = $e10SummaryPath
        historicalSummaryJsonSha256 = if ($identity.e10SummaryExists) { Hash-File $e10SummaryPath } else { $null }
        historicalGateJson = $e10GatePath
        historicalGateJsonSha256 = if ($identity.e10GateExists) { Hash-File $e10GatePath } else { $null }
        historicalStdoutLog = if ($fullSuiteEvidence) { $fullSuiteEvidence.stdoutLog } else { $null }
        historicalStderrLog = if ($fullSuiteEvidence) { $fullSuiteEvidence.stderrLog } else { $null }
        binarySha256 = $script:binaryHash
        runnerSha256 = $toolInfo.runnerSha256
        manifestSha256 = $toolInfo.manifestSha256
        selected = if ($e10) { [int]$e10.counts.selected } else { $null }
        executed = if ($e10) { [int]$e10.counts.executed } else { $null }
        passed = if ($e10) { [int]$e10.counts.passed } else { $null }
        skipped = if ($e10) { [int]$e10.counts.skipped } else { $null }
        failed = if ($e10) { [int]$e10.counts.failed } else { $null }
        errors = if ($e10) { [int]$e10.counts.errors } else { $null }
        selectedIds = if ($e10) { @($e10.selectedIds) } else { @() }
        executedIds = if ($e10) { @($e10.executedIds) } else { @() }
        sourceFingerprintAvailableInHistoricalRecord = $false
        sourceProvenanceLimitation = 'Historical E10 records bind Full to the same binary SHA, commit, dirty flag, host and tool versions, but do not contain a file-level source fingerprint. Current source hash is recorded for this fresh build and cannot be compared to E10.'
        clipboardTouchedByThisRun = $false
        rawOutcomeStatusGroups = if ($e10) { $e10StatusGroups } else { $null }
    }
    Assert-SourceStable 'historical Full identity reconciliation'
} catch {
    $script:failed = $_.Exception.Message
    Write-Error $script:failed -ErrorAction Continue
} finally {
    try { $script:scratchAfter = Get-Scratch } catch { $script:scratchAfter = [ordered]@{ entries = @(); error = $_.Exception.Message } }
    try { if (!$script:sourceAfter) { $script:sourceAfter = Get-SourceHashSet } } catch { $script:sourceAfter = [ordered]@{ error = $_.Exception.Message } }
    try { $script:versionAfter = Hash-File $versionFile } catch { $script:versionAfter = $null }
    try {
        Save-Json (Join-Path $script:run 'source-after.json') $script:sourceAfter
        Save-Json (Join-Path $script:run 'automation-scratch-after.json') $script:scratchAfter
    } catch { Write-Error "Final inventory write failed: $($_.Exception.Message)" -ErrorAction Continue }
    $beforeNames = @($script:scratchBefore.entries | ForEach-Object { $_.path })
    $afterNames = @($script:scratchAfter.entries | ForEach-Object { $_.path })
    $scratchDiff = Compare-Scratch $script:scratchBefore $script:scratchAfter
    $removed = @($scratchDiff.removed)
    $sourceComparisonPerformed = [bool]($script:sourceBefore -and $script:sourceBefore.aggregateSha256 -and $script:sourceAfter -and $script:sourceAfter.aggregateSha256)
    $versionComparisonPerformed = [bool]($script:versionBefore -and $script:versionAfter)
    $sameSource = if ($sourceComparisonPerformed) { $script:sourceBefore.aggregateSha256 -eq $script:sourceAfter.aggregateSha256 } else { $null }
    $sameVersion = if ($versionComparisonPerformed) { $script:versionBefore -eq $script:versionAfter } else { $null }
    $scratchPreserved = !$removed.Count -and !$scratchDiff.changed.Count
    $safeStepsPassed = $summaryCtest -and $summaryCtest.idsReconcile -and $summaryCtest.failed -eq 0
    $status = if ($script:failed -or !$sourceComparisonPerformed -or !$sameSource -or !$versionComparisonPerformed -or !$sameVersion -or !$scratchPreserved) { 'failed' }
        elseif (!$safeStepsPassed -or !$script:fullReuse -or $script:fullReuse.status -ne 'reused_historical_same_binary') { 'pending' }
        else { 'passed' }
    $overallExitCode = if ($status -eq 'failed') {
        if ($null -ne $script:firstNonzeroChildExitCode) { [int]$script:firstNonzeroChildExitCode } else { 1 }
    } elseif ($status -eq 'pending') { 2 } else { 0 }
    $runLogs = @()
    if (Test-Path -LiteralPath (Join-Path $script:run 'logs') -PathType Container) {
        $runLogs = @(Get-ChildItem -LiteralPath (Join-Path $script:run 'logs') -File | Sort-Object Name | ForEach-Object { File-Reference $_.FullName })
    }
    $record = [ordered]@{
        schemaVersion = 1
        runId = $RunId
        startedUtc = $started.ToString('o')
        completedUtc = [DateTime]::UtcNow.ToString('o')
        status = $status
        firstNonzeroChildExitCode = $script:firstNonzeroChildExitCode
        overallExitCode = $overallExitCode
        failure = $script:failed
        baseRevision = $baseRevision
        environmentFile = if (Test-Path (Join-Path $script:run 'environment-before.json') -PathType Leaf) { Join-Path $script:run 'environment-before.json' } else { $null }
        environmentUnverifiedItems = $script:environmentUnverifiedItems
        toolsFile = if (Test-Path (Join-Path $script:run 'tools.json') -PathType Leaf) { Join-Path $script:run 'tools.json' } else { $null }
        sourceBeforeFile = if (Test-Path (Join-Path $script:run 'source-before.json') -PathType Leaf) { Join-Path $script:run 'source-before.json' } else { $null }
        sourceAfterFile = if (Test-Path (Join-Path $script:run 'source-after.json') -PathType Leaf) { Join-Path $script:run 'source-after.json' } else { $null }
        sourceHashBefore = if ($script:sourceBefore) { $script:sourceBefore.aggregateSha256 } else { $null }
        sourceHashAfter = if ($script:sourceAfter -and $script:sourceAfter.aggregateSha256) { $script:sourceAfter.aggregateSha256 } else { $null }
        sourceDirty = $script:sourceDirty
        sourceComparisonPerformed = $sourceComparisonPerformed
        sourceUnchanged = $sameSource
        sourceDriftStoppedRun = $script:sourceDrift
        versionSha256Before = $script:versionBefore
        versionSha256After = $script:versionAfter
        versionComparisonPerformed = $versionComparisonPerformed
        versionUnchanged = $sameVersion
        binaryPath = $script:binary
        binarySha256 = $script:binaryHash
        ctest = $summaryCtest
        fullAutomation = $script:fullReuse
        automationScratchRoot = $scratch
        scratchBeforeFile = if (Test-Path (Join-Path $script:run 'automation-scratch-before.json') -PathType Leaf) { Join-Path $script:run 'automation-scratch-before.json' } else { $null }
        scratchAfterFile = if (Test-Path (Join-Path $script:run 'automation-scratch-after.json') -PathType Leaf) { Join-Path $script:run 'automation-scratch-after.json' } else { $null }
        scratchExistingEntriesPreserved = $scratchPreserved
        scratchRootExistedBefore = if ($script:scratchBefore) { [bool]$script:scratchBefore.exists } else { $null }
        scratchRootExistsAfter = if ($script:scratchAfter) { [bool]$script:scratchAfter.exists } else { $null }
        scratchInventoryDepth = 0
        scratchDirectoryContentHashing = $false
        scratchRemovedEntries = $removed
        scratchChangedEntries = @($scratchDiff.changed)
        scratchAddedEntries = @($scratchDiff.added)
        clipboardAccessed = $false
        clipboardGate = 'OPEN: not probed or mutated by this baseline'
        otherOpenGates = @('supported Windows matrix','visible UI qualification','reference-hardware performance/process lifetime','physical/other printer drivers')
        commands = @($script:commands)
        runDirectory = $script:run
        logsDirectory = Join-Path $script:run 'logs'
        fullAutomationRerun = $false
        fullAutomationEvidenceType = if ($script:fullReuse -and $script:fullReuse.status -eq 'reused_historical_same_binary') {
            'historical same-binary E10 Full evidence reused; not rerun in this session'
        } elseif ($script:fullReuse -and $script:fullReuse.status -eq 'pending_identity_mismatch') {
            $failedReuseChecks = @($script:fullReuse.pendingReasons | ForEach-Object { ($_ -replace '^Historical Full reuse check failed:\s*', '').Trim() } | Where-Object { $_ })
            if ($failedReuseChecks.Count) {
                "historical E10 Full not reused; reuse criteria failed: $($failedReuseChecks -join ', '); Full not rerun"
            } else { 'historical E10 Full not reused; one or more reuse criteria did not pass; Full not rerun' }
        } else { 'no historical E10 Full evidence reused' }
        automationResultFile = if ($script:fullReuse -and $script:fullReuse.status -eq 'reused_historical_same_binary' -and (Test-Path -LiteralPath $script:fullReuse.historicalFullJson -PathType Leaf)) { $script:fullReuse.historicalFullJson } else { $null }
        evidenceReferences = [ordered]@{
            environmentBefore = File-Reference (Join-Path $script:run 'environment-before.json')
            tools = File-Reference (Join-Path $script:run 'tools.json')
            sourceBefore = File-Reference (Join-Path $script:run 'source-before.json')
            sourceAfter = File-Reference (Join-Path $script:run 'source-after.json')
            scratchBefore = File-Reference (Join-Path $script:run 'automation-scratch-before.json')
            scratchAfter = File-Reference (Join-Path $script:run 'automation-scratch-after.json')
            historicalFull = File-Reference $e10FullPath
            historicalSummary = File-Reference $e10SummaryPath
            historicalGate = File-Reference $e10GatePath
            commandLogs = $runLogs
        }
    }
    try { Save-Json $recordPath $record; Write-Host "Baseline record: $recordPath" } catch { Write-Error "Cannot save baseline record: $($_.Exception.Message)" -ErrorAction Continue; $status = 'failed' }
}
if ($status -eq 'failed') {
    if ($null -ne $script:firstNonzeroChildExitCode) { exit ([int]$script:firstNonzeroChildExitCode) }
    exit 1
}
if ($status -eq 'pending') { exit 2 }
exit 0
