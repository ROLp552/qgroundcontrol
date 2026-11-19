# Prepares RadioMaster AX12 radios over USB (adb): installs our QGroundControl APK and protects the
# RadioMaster app from the ELRS Backpack firmware download crash.
#
# The crash: when the AX12 joins the "ExpressLRS TX Backpack" WiFi (captive, no internet), the RadioMaster
# app saves the Backpack's web page as Download/0x00230226a.sfw and crashes decoding it on every start.
# Fix: delete .sfw files that are really web pages and put a folder with that name in Download, so the
# app can't save the file again.
#
# Requirements on each radio: Developer options -> USB debugging ON, connected to this PC with a data cable
# (top USB-C port), and "Allow USB debugging" accepted on the radio.
#
# Usage (from this folder):
#   powershell -ExecutionPolicy Bypass -File .\setup-ax12-radio.ps1
#   powershell -ExecutionPolicy Bypass -File .\setup-ax12-radio.ps1 -Apk C:\qgc-build\APK\QGroundControl-AX12-v10d-branding.apk
#   powershell -ExecutionPolicy Bypass -File .\setup-ax12-radio.ps1 -SkipInstall      # only the RadioMaster fix
#   powershell -ExecutionPolicy Bypass -File .\setup-ax12-radio.ps1 -Once             # one radio, then exit
#
# Note: every AX12 reports the same adb serial (0123456789ABCDEF), so connect one radio at a time.
# Radios are told apart by their Android ID: a radio that was just prepared is not processed again.

param(
    [string]$Apk = 'C:\qgc-build\APK\QGroundControl-AX12-v10d-branding.apk',
    [switch]$SkipInstall,
    [switch]$Once,      # process a single radio and exit
    [string]$Adb = 'C:\Android\Sdk\platform-tools\adb.exe'
)

# 'Continue': adb writes notices (e.g. "daemon not running; starting now") to stderr, which
# PowerShell 5.1 turns into terminating errors under 'Stop'. Real failures are checked explicitly.
$ErrorActionPreference = 'Continue'

$Download    = '/storage/emulated/0/Download'
$BlockName   = '0x00230226a.sfw'
$QgcPackage  = 'org.mavlink.qgroundcontrol'

if (-not (Test-Path $Adb)) { throw "adb not found: $Adb" }
if (-not $SkipInstall -and -not (Test-Path $Apk)) { throw "APK not found: $Apk (use -Apk or -SkipInstall)" }

# Start the adb server up front so its startup notice doesn't mix with the device list
& $Adb start-server 2>&1 | Out-Null

function Invoke-AdbShell([string]$serial, [string]$command) {
    # adb prints warnings on stderr that PowerShell 5.1 turns into errors, keep stdout only
    $output = & $Adb -s $serial shell $command 2>$null
    return ($output | Out-String).Trim()
}

function Wait-Radio {
    Write-Host "`nWaiting for a radio (USB debugging on, cable connected)..." -ForegroundColor Cyan
    while ($true) {
        $lines = (& $Adb devices 2>$null) | Select-Object -Skip 1 | Where-Object { $_ -match '\S' }
        $ready = @($lines | Where-Object { $_ -match '\tdevice$' })
        $unauthorized = @($lines | Where-Object { $_ -match '\tunauthorized$' })
        if ($ready.Count -gt 1) {
            Write-Host 'More than one radio is connected, leave only one.' -ForegroundColor Yellow
        } elseif ($ready.Count -eq 1) {
            return ($ready[0] -split '\t')[0]
        } elseif ($unauthorized.Count -gt 0) {
            Write-Host 'Accept "Allow USB debugging" on the radio screen...' -ForegroundColor Yellow
        }
        Start-Sleep -Seconds 2
    }
}

function Install-Qgc([string]$serial) {
    Write-Host "Installing $(Split-Path $Apk -Leaf) ..."
    $result = (& $Adb -s $serial install -r $Apk 2>&1 | Out-String).Trim()
    if ($result -match 'Success') {
        Write-Host '  QGroundControl installed' -ForegroundColor Green
        return $true
    }
    if ($result -match 'INSTALL_FAILED_UPDATE_INCOMPATIBLE') {
        Write-Host '  An incompatible QGroundControl is installed (e.g. RadioMaster''s build, other signature).' -ForegroundColor Yellow
        Write-Host '  Replacing it deletes its settings (comm links, etc.).' -ForegroundColor Yellow
        $answer = Read-Host '  Uninstall it and install ours? (y/N)'
        if ($answer -match '^[yYsS]') {
            & $Adb -s $serial uninstall $QgcPackage 2>$null | Out-Null
            $result = (& $Adb -s $serial install $Apk 2>&1 | Out-String).Trim()
            if ($result -match 'Success') {
                Write-Host '  QGroundControl installed' -ForegroundColor Green
                return $true
            }
        } else {
            Write-Host '  Skipped QGroundControl install' -ForegroundColor Yellow
            return $false
        }
    }
    Write-Host "  Install failed: $result" -ForegroundColor Red
    return $false
}

function Protect-RadioMasterApp([string]$serial) {
    Write-Host 'Checking Download for fake firmware files...'
    $files = (Invoke-AdbShell $serial "ls -1 $Download 2>/dev/null") -split "`r?`n" | Where-Object { $_ -match '\.sfw$' }
    foreach ($name in $files) {
        $path = "$Download/$name"
        if ((Invoke-AdbShell $serial "[ -d '$path' ] && echo dir") -eq 'dir') {
            continue
        }
        $head = Invoke-AdbShell $serial "head -c 64 '$path'"
        if ($head -match '^\s*<(!DOCTYPE|html)') {
            Invoke-AdbShell $serial "rm -f '$path'" | Out-Null
            Write-Host "  Deleted $name (it was a web page, not firmware)" -ForegroundColor Green
        } else {
            Write-Host "  Kept $name (looks like real firmware)"
        }
    }

    $blockPath = "$Download/$BlockName"
    if ((Invoke-AdbShell $serial "[ -d '$blockPath' ] && echo dir") -ne 'dir') {
        Invoke-AdbShell $serial "mkdir -p '$blockPath'" | Out-Null
    }
    if ((Invoke-AdbShell $serial "[ -d '$blockPath' ] && echo dir") -eq 'dir') {
        Write-Host "  Block folder $BlockName in place" -ForegroundColor Green
        return $true
    }
    Write-Host "  Could not create the block folder $BlockName" -ForegroundColor Red
    return $false
}

$done = 0
$lastRadioId = $null
while ($true) {
    $serial = Wait-Radio
    # Every AX12 has the same adb serial, tell radios apart by their Android ID
    $radioId = Invoke-AdbShell $serial 'settings get secure android_id'
    if ($radioId -and ($radioId -eq $lastRadioId)) {
        Write-Host 'This is the radio that was just prepared, connect the next one...' -ForegroundColor Yellow
        Start-Sleep -Seconds 3
        continue
    }
    $lastRadioId = $radioId
    $model = Invoke-AdbShell $serial 'getprop ro.product.model'
    Write-Host "`n=== Radio $radioId ($model) ===" -ForegroundColor Cyan
    if ($model -ne 'AX12') {
        Write-Host "This is not an AX12 ($model), skipping." -ForegroundColor Yellow
    } else {
        $installed = $SkipInstall -or (Install-Qgc $serial)
        $protected = Protect-RadioMasterApp $serial
        if ($installed -and $protected) {
            $done++
            Write-Host "Radio ready ($done so far). You can unplug it." -ForegroundColor Green
        } else {
            Write-Host 'Radio NOT fully prepared, see the messages above.' -ForegroundColor Red
        }
    }

    if ($Once) {
        break
    }
    $answer = Read-Host "`nConnect the next radio and press Enter (or type Q to quit)"
    if ($answer -match '^[qQ]') {
        break
    }
}
Write-Host "`nDone, $done radio(s) prepared."
