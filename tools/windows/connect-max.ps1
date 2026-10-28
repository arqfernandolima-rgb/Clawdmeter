<#
  connect-max.ps1 - put your personal Claude Max plan on the Clawdmeter's Max screen

  One-time. Signs a second Claude Code login into %USERPROFILE%\.claude-personal.
  Your normal (work) Claude Code login and settings are not touched. The daemon
  picks the new login up on its own within a minute and keeps it signed in.

  Usage:
    powershell -ExecutionPolicy Bypass -File .\connect-max.ps1

  Undo: delete %USERPROFILE%\.claude-personal (the Max screen then drops out
  of the tap cycle after the next board restart).
#>
param([string]$Dir = (Join-Path $env:USERPROFILE '.claude-personal'))

$ErrorActionPreference = 'Stop'
$Here   = $PSScriptRoot
$VenvPy = Join-Path $env:USERPROFILE 'Clawdmeter\.venv\Scripts\python.exe'
$Cred   = Join-Path $Dir '.credentials.json'
$Check  = Join-Path $Here 'clawd_max_check.py'

try {
    if (-not (Test-Path $VenvPy)) { throw "Can't find $VenvPy - run setup-clawdmeter.ps1 first." }
    if (-not (Get-Command claude -ErrorAction SilentlyContinue)) { throw "Can't find claude on PATH." }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null

    if (Test-Path $Cred) {
        Write-Host "A login already exists in $Dir - checking it." -ForegroundColor Cyan
    } else {
        Write-Host ""
        Write-Host "Claude Code will open with a separate, empty profile:" -ForegroundColor Cyan
        Write-Host "  1. Pick a theme, then choose 'Claude account with subscription'"
        Write-Host "     (if it skips straight to a chat prompt, type /login)"
        Write-Host "  2. Sign in with your PERSONAL account (the one with Max) in the browser."
        Write-Host "     If the browser shows your work account, log out there first, or paste"
        Write-Host "     the link Claude Code prints into a private window."
        Write-Host "  3. When you see the chat prompt, type /exit"
        Write-Host ""
        Read-Host "Press Enter to open Claude Code"
        $old = $env:CLAUDE_CONFIG_DIR
        $env:CLAUDE_CONFIG_DIR = $Dir
        try { & claude }
        finally {
            if ($old) { $env:CLAUDE_CONFIG_DIR = $old }
            else { Remove-Item Env:\CLAUDE_CONFIG_DIR -ErrorAction SilentlyContinue }
        }
        if (-not (Test-Path $Cred)) { throw "No login saved in $Dir. Run this again and finish the sign-in (/login)." }
    }

    Write-Host ""
    & $VenvPy $Check $Dir
    if ($LASTEXITCODE -ne 0) { throw "The check above needs a look." }

    Write-Host ""
    Write-Host "Done. Within a minute, tap the board: Clawd -> Usage -> Max -> Today." -ForegroundColor Green
    Write-Host "To use Claude Code on this account yourself later:  `$env:CLAUDE_CONFIG_DIR = '$Dir'; claude"
}
catch {
    Write-Host ""
    Write-Host "STOPPED: $($_.Exception.Message)" -ForegroundColor Red
}
