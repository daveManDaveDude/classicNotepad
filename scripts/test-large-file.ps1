param(
    [string]$Binary = (Join-Path $PSScriptRoot "..\build\Release\ClassicNotepad.exe"),
    [int]$MaxFirstUsableMs = 2000
)

$ErrorActionPreference = "Stop"
$python = Get-Command python -ErrorAction Stop
& $python.Source (Join-Path $PSScriptRoot "..\tests\automation\large_file_performance.py") `
    --binary $Binary `
    --max-first-usable-ms $MaxFirstUsableMs
if ($LASTEXITCODE -ne 0) {
    throw "The 700 MiB large-file performance test failed."
}
