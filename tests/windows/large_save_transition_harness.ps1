param(
    [Parameter(Mandatory = $true)][string]$Application,
    [string]$WorkDirectory = (Join-Path $env:TEMP ("classic-notepad-a2-" + [guid]::NewGuid().ToString("N"))),
    [ValidateSet("all", "new", "open", "close", "cancel", "replace", "edit-during-save", "busy-save", "faults")][string]$Scenario = "all"
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class A2Win32 {
    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    public delegate bool EnumChildWindowsProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumChildWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string title);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dialog, int id);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool SetWindowText(IntPtr window, string text);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr OpenEvent(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetEvent(IntPtr handle);
    [DllImport("kernel32.dll", SetLastError=true)] public static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
    public static IntPtr[] WindowsForProcess(int processId) {
        var result = new System.Collections.Generic.List<IntPtr>();
        EnumWindows((window, parameter) => {
            uint foundProcessId;
            GetWindowThreadProcessId(window, out foundProcessId);
            if (foundProcessId == processId && IsWindowVisible(window)) result.Add(window);
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
    public static string DialogText(IntPtr dialog) {
        var parts = new System.Collections.Generic.List<string>();
        var text = new StringBuilder(2048);
        GetWindowText(dialog, text, text.Capacity);
        parts.Add(text.ToString());
        EnumChildWindows(dialog, (window, parameter) => {
            var childText = new StringBuilder(2048);
            GetWindowText(window, childText, childText.Capacity);
            if (!String.IsNullOrWhiteSpace(childText.ToString())) parts.Add(childText.ToString());
            return true;
        }, IntPtr.Zero);
        return String.Join("\n", parts);
    }
    public static IntPtr FindChildClass(IntPtr parent, string className) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (window, parameter) => {
            var childClass = new StringBuilder(256);
            GetClassName(window, childClass, childClass.Capacity);
            if (String.Equals(childClass.ToString(), className, StringComparison.OrdinalIgnoreCase)) {
                found = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr FindChildId(IntPtr parent, int id) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (window, parameter) => {
            if (GetDlgCtrlID(window) == id) {
                found = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr FindChildButtonText(IntPtr parent, string buttonText) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (window, parameter) => {
            var childClass = new StringBuilder(256);
            var childText = new StringBuilder(512);
            GetClassName(window, childClass, childClass.Capacity);
            GetWindowText(window, childText, childText.Capacity);
            if (String.Equals(childClass.ToString(), "Button", StringComparison.OrdinalIgnoreCase) &&
                String.Equals(childText.ToString().Trim('&'), buttonText, StringComparison.OrdinalIgnoreCase)) {
                found = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

$WM_COMMAND = 0x0111
$WM_CHAR = 0x0102
$WM_CLOSE = 0x0010
$BM_CLICK = 0x00F5
$ID_FILE_NEW = 40001
$ID_FILE_OPEN = 40002
$ID_FILE_SAVE = 40003
$IDYES = 6
$IDCANCEL = 2

function Get-WindowText([IntPtr]$Window) {
    $buffer = [System.Text.StringBuilder]::new(2048)
    [void][A2Win32]::GetWindowText($Window, $buffer, $buffer.Capacity)
    return $buffer.ToString()
}

function Get-WindowClass([IntPtr]$Window) {
    $buffer = [System.Text.StringBuilder]::new(256)
    [void][A2Win32]::GetClassName($Window, $buffer, $buffer.Capacity)
    return $buffer.ToString()
}

function Get-DialogText([IntPtr]$Dialog) {
    return [A2Win32]::DialogText($Dialog)
}

function Get-ProcessWindows([int]$ProcessId) {
    return [A2Win32]::WindowsForProcess($ProcessId)
}

function Wait-ForWindow([int]$ProcessId, [scriptblock]$Predicate, [int]$TimeoutSeconds = 30) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $nextDiagnostic = [DateTime]::UtcNow.AddSeconds(2)
    while ([DateTime]::UtcNow -lt $deadline) {
        $windows = Get-ProcessWindows $ProcessId
        foreach ($window in $windows) {
            if (& $Predicate $window) { return $window }
        }
        if ([DateTime]::UtcNow -ge $nextDiagnostic) {
            $details = foreach ($window in $windows) { "$(Get-WindowClass $window):$(Get-DialogText $window)" }
            Write-Host "Waiting on PID $ProcessId; windows: $($details -join ' | ')"
            $nextDiagnostic = [DateTime]::UtcNow.AddSeconds(2)
        }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out waiting for a window owned by process $ProcessId."
}

function Get-MainWindow($Process) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        $Process.Refresh()
        $window = $Process.MainWindowHandle
        if ($window -ne [IntPtr]::Zero) {
            $title = Get-WindowText $window
            if ((Get-WindowClass $window) -ne "#32770" -and $title.Contains("Classic Notepad")) { return $window }
        }
        Start-Sleep -Milliseconds 50
    }
    throw "Timed out waiting for the main application window (PID $($Process.Id))."
}

function Wait-ForMessageBox([int]$ProcessId, [int]$ButtonId, [int]$TimeoutSeconds = 30) {
    return Wait-ForWindow $ProcessId {
        param($window)
        $class = Get-WindowClass $window
        return ($class -eq "ClassicNotepadMessageDialog" -or $class -eq "#32770") -and
            [A2Win32]::GetDlgItem($window, $ButtonId) -ne [IntPtr]::Zero
    } $TimeoutSeconds
}

function Click-DialogButton([IntPtr]$Dialog, [int]$ButtonId) {
    $button = [A2Win32]::GetDlgItem($Dialog, $ButtonId)
    if ($button -eq [IntPtr]::Zero) { throw "Dialog button $ButtonId was not found." }
    [void][A2Win32]::SendMessage($button, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
}

function New-LargeFixture([string]$Path, [string]$ExpectedPath) {
    $line = [System.Text.Encoding]::ASCII.GetBytes(("0123456789abcdef" * 255) + "`r`n")
    $targetBytes = 52MB
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
    try {
        while ($stream.Length + $line.Length -le $targetBytes) { $stream.Write($line, 0, $line.Length) }
    } finally { $stream.Dispose() }
    $input = [System.IO.File]::OpenRead($Path)
    $expected = [System.IO.File]::Create($ExpectedPath)
    try {
        $prefix = [byte[]](0x58)
        $expected.Write($prefix, 0, 1)
        $input.CopyTo($expected)
    } finally { $expected.Dispose(); $input.Dispose() }
}

function Get-Sha256([string]$Path) {
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $input = $null
        try {
            $input = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
            return ([BitConverter]::ToString($sha.ComputeHash($input))).Replace("-", "")
        } catch [System.IO.IOException] {
            if ($attempt -eq 39) { throw }
            Start-Sleep -Milliseconds 50
        } finally {
            if ($null -ne $input) { $input.Dispose() }
            $sha.Dispose()
        }
    }
}

function Start-TestApplication([string]$Path, [bool]$PauseAfterSnapshot = $false, [string]$FaultStage = "") {
    $previousGate = $env:CLASSIC_NOTEPAD_TEST_SAVE_GATE
    $previousFault = $env:CLASSIC_NOTEPAD_TEST_SAVE_FAULT
    try {
        if ($PauseAfterSnapshot) { $env:CLASSIC_NOTEPAD_TEST_SAVE_GATE = "1" }
        if ($FaultStage) { $env:CLASSIC_NOTEPAD_TEST_SAVE_FAULT = $FaultStage }
        return Start-Process -FilePath $Application -ArgumentList ('"' + $Path + '"') -PassThru
    } finally {
        if ($null -eq $previousGate) { Remove-Item Env:CLASSIC_NOTEPAD_TEST_SAVE_GATE -ErrorAction SilentlyContinue }
        else { $env:CLASSIC_NOTEPAD_TEST_SAVE_GATE = $previousGate }
        if ($null -eq $previousFault) { Remove-Item Env:CLASSIC_NOTEPAD_TEST_SAVE_FAULT -ErrorAction SilentlyContinue }
        else { $env:CLASSIC_NOTEPAD_TEST_SAVE_FAULT = $previousFault }
    }
}

function Invoke-SaveFaultScenario([string]$Stage) {
    $source = Join-Path $WorkDirectory ("fault-$Stage-source.txt")
    $expected = Join-Path $WorkDirectory ("fault-$Stage-expected.txt")
    New-LargeFixture $source $expected
    $originalHash = Get-Sha256 $source
    $process = Start-TestApplication $source $false $Stage
    try {
        Write-Output "RUN injected Save $Stage failure"
        $window = Get-MainWindow $process
        Start-Sleep -Seconds 8
        Send-Edit $window
        [void][A2Win32]::PostMessage($window, $WM_COMMAND, [IntPtr]$ID_FILE_NEW, [IntPtr]::Zero)
        $prompt = Wait-ForMessageBox $process.Id $IDYES
        Click-DialogButton $prompt $IDYES
        $failure = Wait-ForMessageBox $process.Id 1 45
        Click-DialogButton $failure 1
        Assert-LargeViewDirty (Get-MainWindow $process) $Stage
        if ((Get-LargeView (Get-MainWindow $process)) -eq [IntPtr]::Zero) { throw "$Stage failure discarded the source view." }
        if ((Get-Sha256 $source) -ne $originalHash) { throw "$Stage failure changed the original destination." }
        if ($Stage -eq "replace") {
            $recoverable = Get-ChildItem -LiteralPath $WorkDirectory -Filter "fault-$Stage-source.txt.classic-notepad-save-*.tmp" | Select-Object -First 1
            if ($null -eq $recoverable -or (Get-Sha256 $recoverable.FullName) -ne (Get-Sha256 $expected)) {
                throw "Replace failure did not retain the full recoverable output."
            }
        }
        Send-Edit (Get-MainWindow $process)
        [void][A2Win32]::PostMessage((Get-MainWindow $process), $WM_COMMAND, [IntPtr]$ID_FILE_NEW, [IntPtr]::Zero)
        $secondPrompt = Wait-ForMessageBox $process.Id $IDYES
        Click-DialogButton $secondPrompt $IDCANCEL
        if ((Get-LargeView (Get-MainWindow $process)) -eq [IntPtr]::Zero) { throw "$Stage failure left the document unreadable or discarded." }
        Write-Output "PASS $Stage failure keeps the large document readable and dirty; its destination/recovery state is correct. Error dialog appeared; custom-drawn text is not asserted."
    } finally { Stop-TestApplication $process }
}

function Wait-SaveSnapshotGate([int]$ProcessId) {
    $name = "Local\ClassicNotepadA2SaveGate-$ProcessId-ready"
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline) {
        $handle = [A2Win32]::OpenEvent(0x00100000, $false, $name)
        if ($handle -ne [IntPtr]::Zero) {
            try {
                if ([A2Win32]::WaitForSingleObject($handle, 0) -eq 0) { return }
            } finally { [void][A2Win32]::CloseHandle($handle) }
        }
        Start-Sleep -Milliseconds 25
    }
    throw "Timed out waiting for the deterministic save snapshot gate."
}

function Release-SaveSnapshotGate([int]$ProcessId) {
    $name = "Local\ClassicNotepadA2SaveGate-$ProcessId-proceed"
    $handle = [A2Win32]::OpenEvent(0x0002, $false, $name)
    if ($handle -eq [IntPtr]::Zero) { throw "Could not open the save snapshot release event." }
    try { if (-not [A2Win32]::SetEvent($handle)) { throw "Could not release the save snapshot gate." } }
    finally { [void][A2Win32]::CloseHandle($handle) }
}

function Get-LargeView([IntPtr]$MainWindow) {
    return [A2Win32]::FindWindowEx($MainWindow, [IntPtr]::Zero, "ClassicNotepadLargeFileView", $null)
}

function Assert-LargeViewDirty([IntPtr]$MainWindow, [string]$Stage) {
    $title = Get-WindowText $MainWindow
    if (-not $title.StartsWith("*", [System.StringComparison]::Ordinal)) {
        throw "$Stage failure did not leave the document dirty immediately after the error. Title: $title"
    }
}

function Send-Edit([IntPtr]$MainWindow) {
    $view = Get-LargeView $MainWindow
    if ($view -eq [IntPtr]::Zero) { throw "Large-file view was not created." }
    [void][A2Win32]::SendMessage($view, $WM_CHAR, [IntPtr]([int][char]'X'), [IntPtr]::Zero)
}

function Stop-TestApplication($Process) {
    if (-not $Process.HasExited) {
        try { $Process.Kill(); $Process.WaitForExit() } catch { }
    }
}

New-Item -ItemType Directory -Path $WorkDirectory -Force | Out-Null
try {
    if ($Scenario -in @("all", "new")) {
    $newSource = Join-Path $WorkDirectory "new-source.txt"
    $newExpected = Join-Path $WorkDirectory "new-expected.txt"
    New-LargeFixture $newSource $newExpected
    $newProcess = Start-TestApplication $newSource
    try {
        Write-Output "RUN New transition"
        $newWindow = Get-MainWindow $newProcess
        Start-Sleep -Seconds 8
        Send-Edit $newWindow
        [void][A2Win32]::PostMessage($newWindow, $WM_COMMAND, [IntPtr]$ID_FILE_NEW, [IntPtr]::Zero)
        $prompt = Wait-ForMessageBox $newProcess.Id $IDYES
        Click-DialogButton $prompt $IDYES
        $newWindow = Wait-ForWindow $newProcess.Id { param($window) (Get-WindowText $window) -eq "Untitled - Classic Notepad" } 45
        if ((Get-Sha256 $newSource) -ne (Get-Sha256 $newExpected)) { throw "New transitioned before the complete edited bytes were saved." }
        Write-Output "PASS New waits for the successful large-file Save; destination hash matches the complete edited document."
    } finally { Stop-TestApplication $newProcess }
    }

    if ($Scenario -in @("all", "open")) {
    $openSource = Join-Path $WorkDirectory "open-source.txt"
    $openExpected = Join-Path $WorkDirectory "open-expected.txt"
    New-LargeFixture $openSource $openExpected
    $openProcess = Start-TestApplication $openSource
    try {
        Write-Output "RUN Open transition"
        $openWindow = Get-MainWindow $openProcess
        Start-Sleep -Seconds 8
        Send-Edit $openWindow
        [void][A2Win32]::PostMessage($openWindow, $WM_COMMAND, [IntPtr]$ID_FILE_OPEN, [IntPtr]::Zero)
        $prompt = Wait-ForMessageBox $openProcess.Id $IDYES
        Click-DialogButton $prompt $IDYES
        $openDialog = Wait-ForWindow $openProcess.Id { param($window) (Get-WindowClass $window) -eq "#32770" -and (Get-WindowText $window) -eq "Open" } 45
        if ((Get-Sha256 $openSource) -ne (Get-Sha256 $openExpected)) { throw "Open dialog appeared before the complete edited bytes were saved." }
        [void][A2Win32]::PostMessage($openDialog, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 250
        if ([A2Win32]::IsWindow($openDialog)) { throw "The Open picker remained open after Cancel." }
        if ((Get-LargeView (Get-MainWindow $openProcess)) -eq [IntPtr]::Zero) { throw "Canceling Open discarded the saved large document." }
        Write-Output "PASS Open remains queued until the complete edited bytes are saved; canceling the file picker retains the saved source view."
    } finally { Stop-TestApplication $openProcess }
    }

    if ($Scenario -in @("all", "close")) {
    $closeSource = Join-Path $WorkDirectory "close-source.txt"
    $closeExpected = Join-Path $WorkDirectory "close-expected.txt"
    New-LargeFixture $closeSource $closeExpected
    $closeProcess = Start-TestApplication $closeSource
    try {
        Write-Output "RUN immediate Close transition"
        $closeWindow = Get-MainWindow $closeProcess
        Start-Sleep -Seconds 8
        Send-Edit $closeWindow
        [void][A2Win32]::PostMessage($closeWindow, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
        $prompt = Wait-ForMessageBox $closeProcess.Id $IDYES
        Click-DialogButton $prompt $IDYES
        if (-not $closeProcess.WaitForExit(45000)) { throw "Close did not complete after the save finished." }
        if ((Get-Sha256 $closeSource) -ne (Get-Sha256 $closeExpected)) { throw "Close discarded or truncated the immediately edited document." }
        Write-Output "PASS Close immediately after a keystroke waits for the complete successful Save."
    } finally { Stop-TestApplication $closeProcess }
    }

    if ($Scenario -in @("all", "cancel")) {
    $cancelSource = Join-Path $WorkDirectory "cancel-source.txt"
    $cancelExpected = Join-Path $WorkDirectory "cancel-expected.txt"
    New-LargeFixture $cancelSource $cancelExpected
    $cancelOriginalHash = Get-Sha256 $cancelSource
    $cancelProcess = Start-TestApplication $cancelSource
    try {
        Write-Output "RUN Cancel prompt"
        $cancelWindow = Get-MainWindow $cancelProcess
        Start-Sleep -Seconds 8
        Send-Edit $cancelWindow
        [void][A2Win32]::PostMessage($cancelWindow, $WM_COMMAND, [IntPtr]$ID_FILE_NEW, [IntPtr]::Zero)
        $prompt = Wait-ForMessageBox $cancelProcess.Id $IDCANCEL
        Click-DialogButton $prompt $IDCANCEL
        Start-Sleep -Milliseconds 250
        if ((Get-LargeView (Get-MainWindow $cancelProcess)) -eq [IntPtr]::Zero) { throw "Cancel discarded the large document." }
        if ((Get-Sha256 $cancelSource) -ne $cancelOriginalHash) { throw "Cancel wrote to the source file." }
        Write-Output "PASS Cancel retains the dirty large document and leaves disk bytes unchanged."
    } finally { Stop-TestApplication $cancelProcess }
    }

    if ($Scenario -in @("all", "replace")) {
    $blockedSource = Join-Path $WorkDirectory "blocked-source.txt"
    $blockedExpected = Join-Path $WorkDirectory "blocked-expected.txt"
    New-LargeFixture $blockedSource $blockedExpected
    $blockedOriginalHash = Get-Sha256 $blockedSource
    $blockedProcess = Start-TestApplication $blockedSource
    $lock = $null
    try {
        Write-Output "RUN Replace failure recovery"
        $blockedWindow = Get-MainWindow $blockedProcess
        Start-Sleep -Seconds 8
        Send-Edit $blockedWindow
        $lock = [System.IO.File]::Open($blockedSource, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::ReadWrite)
        [void][A2Win32]::PostMessage($blockedWindow, $WM_COMMAND, [IntPtr]$ID_FILE_NEW, [IntPtr]::Zero)
        $prompt = Wait-ForMessageBox $blockedProcess.Id $IDYES
        Click-DialogButton $prompt $IDYES
        $failure = Wait-ForMessageBox $blockedProcess.Id 1 45
        Click-DialogButton $failure 1
        Assert-LargeViewDirty (Get-MainWindow $blockedProcess) "replace"
        $lock.Dispose()
        $lock = $null
        if ((Get-LargeView (Get-MainWindow $blockedProcess)) -eq [IntPtr]::Zero) { throw "Replace failure discarded the source view." }
        if ((Get-Sha256 $blockedSource) -ne $blockedOriginalHash) { throw "Replace failure changed the original destination." }
        $recoverable = Get-ChildItem -LiteralPath $WorkDirectory -Filter "blocked-source.txt.classic-notepad-save-*.tmp" | Select-Object -First 1
        if ($null -eq $recoverable) { throw "Replace failure did not retain the complete temporary output." }
        if ((Get-Sha256 $recoverable.FullName) -ne (Get-Sha256 $blockedExpected)) { throw "Retained temporary output is incomplete." }
        Write-Output "PASS Injected destination sharing failure shows an error dialog, keeps the source view, and retains complete recoverable output. The harness does not inspect custom-drawn dialog text."
    } finally {
        if ($null -ne $lock) { $lock.Dispose() }
        Stop-TestApplication $blockedProcess
    }
    }

    if ($Scenario -eq "edit-during-save") {
    $raceSource = Join-Path $WorkDirectory "race-source.txt"
    $raceExpected = Join-Path $WorkDirectory "race-expected.txt"
    New-LargeFixture $raceSource $raceExpected
    $raceOriginalHash = Get-Sha256 $raceSource
    $raceProcess = Start-TestApplication $raceSource $true
    try {
        Write-Output "RUN deterministic edit during Save"
        $raceWindow = Get-MainWindow $raceProcess
        Start-Sleep -Seconds 8
        Send-Edit $raceWindow
        [void][A2Win32]::PostMessage($raceWindow, $WM_COMMAND, [IntPtr]$ID_FILE_NEW, [IntPtr]::Zero)
        $prompt = Wait-ForMessageBox $raceProcess.Id $IDYES
        Click-DialogButton $prompt $IDYES
        Wait-SaveSnapshotGate $raceProcess.Id
        Send-Edit (Get-MainWindow $raceProcess)
        Release-SaveSnapshotGate $raceProcess.Id
        $failure = Wait-ForMessageBox $raceProcess.Id 1 45
        Click-DialogButton $failure 1
        if ((Get-LargeView (Get-MainWindow $raceProcess)) -eq [IntPtr]::Zero) { throw "The stale Save transitioned away from the edited source." }
        if ((Get-Sha256 $raceSource) -ne $raceOriginalHash) { throw "The stale snapshot replaced the source despite a newer edit." }
        [void][A2Win32]::PostMessage((Get-MainWindow $raceProcess), $WM_COMMAND, [IntPtr]$ID_FILE_NEW, [IntPtr]::Zero)
        $secondPrompt = Wait-ForMessageBox $raceProcess.Id $IDYES
        Click-DialogButton $secondPrompt $IDCANCEL
        if ((Get-LargeView (Get-MainWindow $raceProcess)) -eq [IntPtr]::Zero) { throw "Canceling after the raced edit discarded the large document." }
        Write-Output "PASS An edit after snapshot completion is detected by the final locked revision check; New remains queued, disk is unchanged, and the newer document remains dirty."
    } finally { Stop-TestApplication $raceProcess }
    }

    if ($Scenario -eq "busy-save") {
    $busySource = Join-Path $WorkDirectory "busy-source.txt"
    $busyExpected = Join-Path $WorkDirectory "busy-expected.txt"
    New-LargeFixture $busySource $busyExpected
    $busyOriginalHash = Get-Sha256 $busySource
    $busyProcess = Start-TestApplication $busySource $true
    try {
        Write-Output "RUN New request during an active large Save"
        $busyWindow = Get-MainWindow $busyProcess
        Start-Sleep -Seconds 8
        Send-Edit $busyWindow
        [void][A2Win32]::PostMessage($busyWindow, $WM_COMMAND, [IntPtr]$ID_FILE_SAVE, [IntPtr]::Zero)
        Wait-SaveSnapshotGate $busyProcess.Id
        [void][A2Win32]::PostMessage($busyWindow, $WM_COMMAND, [IntPtr]$ID_FILE_NEW, [IntPtr]::Zero)
        $busyDialog = Wait-ForMessageBox $busyProcess.Id 1
        Click-DialogButton $busyDialog 1
        if ((Get-LargeView (Get-MainWindow $busyProcess)) -eq [IntPtr]::Zero) { throw "New transitioned while Save was still running." }
        Release-SaveSnapshotGate $busyProcess.Id
        $deadline = [DateTime]::UtcNow.AddSeconds(45)
        while ([DateTime]::UtcNow -lt $deadline -and (Get-Sha256 $busySource) -ne (Get-Sha256 $busyExpected)) {
            Start-Sleep -Milliseconds 100
        }
        if ((Get-Sha256 $busySource) -ne (Get-Sha256 $busyExpected)) { throw "The active Save did not write the complete edited document." }
        if ((Get-LargeView (Get-MainWindow $busyProcess)) -eq [IntPtr]::Zero) { throw "The rejected busy New request discarded the source view." }
        if ($busyOriginalHash -eq (Get-Sha256 $busyExpected)) { throw "Busy Save fixture did not contain an edit." }
        Write-Output "PASS New is rejected while a large Save is active; the Save then completes without running the rejected transition."
    } finally { Stop-TestApplication $busyProcess }
    }

    if ($Scenario -eq "faults") {
        foreach ($stage in @("write", "flush", "replace")) { Invoke-SaveFaultScenario $stage }
        Write-Output "NOTE Reopen-failure recovery is covered by the synchronous large-file automation test (test_synchronous_large_file_save_faults_keep_dirty_source_readable); the native transition path uses asynchronous Save and does not reopen the committed output."
    }
}
finally {
    $resolved = [System.IO.Path]::GetFullPath($WorkDirectory)
    $tempRoot = [System.IO.Path]::GetFullPath($env:TEMP)
    if ($resolved.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path $resolved -Leaf).StartsWith("classic-notepad-a2-", [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
    }
}
