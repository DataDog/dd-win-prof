# Install Python dependencies required by the integration tests.
#
# Usage:
#   .\install-dependencies.ps1
#   .\install-dependencies.ps1 -PythonCmd "python3"

[CmdletBinding()]
param(
    [string]$PythonCmd = ""
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

# Resolve Python command: try py -3 first (Windows launcher), then python
if (-not $PythonCmd) {
    $candidates = @(
        @{ Cmd = "py"; Args = @("-3", "--version") },
        @{ Cmd = "python"; Args = @("--version") }
    )
    foreach ($c in $candidates) {
        try {
            $ver = & $c.Cmd @($c.Args) 2>&1
            if ($LASTEXITCODE -eq 0 -and $ver -match "Python 3") {
                if ($c.Cmd -eq "py") { $PythonCmd = "py -3" } else { $PythonCmd = $c.Cmd }
                break
            }
        } catch {}
    }
}

if (-not $PythonCmd) {
    Write-Host "ERROR: Python 3 not found. Install Python 3 from https://www.python.org/downloads/" -ForegroundColor Red
    exit 1
}

Write-Host "Using Python: $PythonCmd" -ForegroundColor Cyan

$reqFile = Join-Path $scriptDir "requirements.txt"
Write-Host "Installing packages from $reqFile ..." -ForegroundColor Cyan

$pipArgs = @("-m", "pip", "install", "-r", $reqFile)
Invoke-Expression "$PythonCmd $($pipArgs -join ' ')"
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: pip install failed (exit code $LASTEXITCODE)" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "Dependencies installed successfully." -ForegroundColor Green
