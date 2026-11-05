<#
  fix-autostart.ps1 - make sure the Clawdmeter daemon starts with Windows

  1. Reports what's there now: the "Clawdmeter" startup entry, whether Windows
     has it switched off under Startup apps, whether the tray app is running,
     and when the laptop last booted.
  2. Fixes it:
     - rewrites the startup entry (HKCU Run) with the right command
     - switches it back on if Windows' Startup apps had it off
     - adds a per-user scheduled task as a backup: at sign-in (after 30 s) and
       every 15 minutes it starts the tray app if it isn't running. The app
       allows only one copy, so the extra starts do nothing when it's up.
  3. Starts the tray app now if it isn't running.

  No admin rights needed. Usage:
    powershell -ExecutionPolicy Bypass -File .\fix-autostart.ps1
  Undo: Unregister-ScheduledTask -TaskName Clawdmeter -Confirm:$false
#>
$ErrorActionPreference = 'Stop'
$Repo   = Join-Path $env:USERPROFILE 'Clawdmeter'
$Tray   = Join-Path $Repo 'daemon\tray_windows.py'
$VenvPy = Join-Path $Repo '.venv\Scripts\python.exe'
$RunKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$Approved = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run'
$Name   = 'Clawdmeter'

function Step($t) { Write-Host ""; Write-Host "==> $t" -ForegroundColor Cyan }
function Ok($t)   { Write-Host "    OK  $t" -ForegroundColor Green }
function Warn($t) { Write-Host "    !!  $t" -ForegroundColor Yellow }

function TrayProcs { @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*tray_windows.py*' }) }

try {
    if (-not (Test-Path $Tray))   { throw "Can't find $Tray" }
    if (-not (Test-Path $VenvPy)) { throw "Can't find $VenvPy" }
    $BasePythonw = Join-Path ((& $VenvPy -c "import sys; print(sys.base_exec_prefix)").Trim()) 'pythonw.exe'
    if (-not (Test-Path $BasePythonw)) { throw "Can't find $BasePythonw" }
    $Cmd = "`"$BasePythonw`" `"$Tray`""

    # ---------------------------------------------------------------- report
    Step "What's there now"
    Write-Host ("    Last boot:        " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime)
    $procs = TrayProcs
    $since = ($procs | ForEach-Object { $_.CreationDate }) -join ', '
    if ($procs.Count) { Write-Host "    Tray app:         running since $since" }
    else              { Write-Host  "    Tray app:         NOT running" }
    $cur = (Get-ItemProperty -Path $RunKey -Name $Name -ErrorAction SilentlyContinue).$Name
    if ($cur) { Write-Host "    Startup entry:    $cur" } else { Write-Host "    Startup entry:    MISSING" }
    $flag = (Get-ItemProperty -Path $Approved -Name $Name -ErrorAction SilentlyContinue).$Name
    if ($flag -and ($flag[0] % 2) -eq 1) { Write-Host "    Startup apps:     switched OFF in Windows (Task Manager > Startup apps)" }
    elseif ($flag)                       { Write-Host "    Startup apps:     on" }
    $task = Get-ScheduledTask -TaskName $Name -ErrorAction SilentlyContinue
    if ($task) { Write-Host "    Scheduled task:   present ($($task.State))" } else { Write-Host "    Scheduled task:   none" }

    # ---------------------------------------------------------------- fix
    Step "Startup entry"
    New-Item -Path $RunKey -Force | Out-Null
    Set-ItemProperty -Path $RunKey -Name $Name -Value $Cmd
    Ok "set to $Cmd"
    if ($flag -and ($flag[0] % 2) -eq 1) {
        New-Item -Path $Approved -Force | Out-Null
        Set-ItemProperty -Path $Approved -Name $Name -Type Binary -Value ([byte[]](2,0,0,0,0,0,0,0,0,0,0,0))
        Ok "switched back on in Startup apps"
    }

    Step "Backup scheduled task (sign-in + every 15 min, only starts it if it isn't running)"
    try {
        $action = New-ScheduledTaskAction -Execute $BasePythonw -Argument "`"$Tray`"" -WorkingDirectory $Repo
        $atLogon = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
        $atLogon.Delay = 'PT30S'
        $every = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(2) `
                    -RepetitionInterval (New-TimeSpan -Minutes 15)
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                    -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Seconds 0) -MultipleInstances IgnoreNew
        $principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited
        Register-ScheduledTask -TaskName $Name -Action $action -Trigger @($atLogon, $every) `
            -Settings $settings -Principal $principal -Force | Out-Null
        Ok "registered (Task Scheduler > Task Scheduler Library > $Name)"
    } catch {
        Warn "couldn't add the scheduled task ($($_.Exception.Message)); the startup entry alone still works"
    }

    Step "Running now?"
    if ((TrayProcs).Count) { Ok "already running" }
    else {
        Start-Process -FilePath $BasePythonw -ArgumentList "`"$Tray`"" -WorkingDirectory $Repo
        Start-Sleep -Seconds 5
        if ((TrayProcs).Count) { Ok "started" } else { Warn "didn't start; check $env:LOCALAPPDATA\Clawdmeter\daemon.log" }
    }

    Write-Host ""
    Write-Host "Done. It will start on its own after the next restart or sign-in." -ForegroundColor Green
}
catch {
    Write-Host ""
    Write-Host "STOPPED: $($_.Exception.Message)" -ForegroundColor Red
}
