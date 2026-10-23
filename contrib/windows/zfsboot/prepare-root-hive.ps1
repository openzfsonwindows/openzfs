# SPDX-License-Identifier: CDDL-1.0
<#
.SYNOPSIS
  Prepare a copy of a SYSTEM registry hive for booting from a ZFS root.

.DESCRIPTION
  Loads the hive offline, makes OpenZFS a boot-start file system driver, tells
  it to import the pool the system was booted from, and removes the drive
  letter assignments so that the ZFS volume can take C:. The hive is edited in
  place, so work on a copy (reg save HKLM\SYSTEM <file> /y on the running
  system, or the file from the install you are copying).

  What it changes, in the current control set:
    Services\OpenZFS            Start=0, Group="Boot File System",
                                ImagePath=\SystemRoot\System32\drivers\OpenZFS.sys
    Services\OpenZFS\zfs        boot_import=2 (QWORD): import only the pool on
                                the partition that was booted (1 = any ZFS
                                volume, 0 = off)
    Control\CrashControl        AutoReboot=0, so a bugcheck stays on screen
    MountedDevices              every value removed

.EXAMPLE
  reg save HKLM\SYSTEM C:\work\SYSTEM /y
  .\prepare-root-hive.ps1 -Hive C:\work\SYSTEM
  Copy-Item C:\work\SYSTEM Y:\Windows\System32\config\SYSTEM -Force
#>
param(
    [Parameter(Mandatory = $true)][string]$Hive,
    [string]$Mount = 'ZSYS',
    [switch]$KeepAutoReboot
)

$ErrorActionPreference = 'Stop'

reg load "HKLM\$Mount" $Hive | Out-Null
try {
    $cur = (Get-ItemProperty "HKLM:\$Mount\Select").Current
    $cs = 'ControlSet{0:D3}' -f $cur
    $k = "HKLM\$Mount\$cs\Services\OpenZFS"

    reg add $k /v Type /t REG_DWORD /d 2 /f | Out-Null
    reg add $k /v ErrorControl /t REG_DWORD /d 1 /f | Out-Null
    reg add $k /v Start /t REG_DWORD /d 0 /f | Out-Null
    reg add $k /v Group /t REG_SZ /d 'Boot File System' /f | Out-Null
    reg add $k /v ImagePath /t REG_EXPAND_SZ `
        /d '\SystemRoot\System32\drivers\OpenZFS.sys' /f | Out-Null
    # QWORD: a DWORD in a 64 bit tunable's slot used to crash the driver.
    reg add "$k\zfs" /v boot_import /t REG_QWORD /d 2 /f | Out-Null

    if (-not $KeepAutoReboot) {
        reg add "HKLM\$Mount\$cs\Control\CrashControl" /v AutoReboot `
            /t REG_DWORD /d 0 /f | Out-Null
    }

    reg delete "HKLM\$Mount\MountedDevices" /va /f 2>$null | Out-Null

    "Edited $cs in $Hive"
    reg query $k /v Start
    reg query $k /v Group
    reg query "$k\zfs" /v boot_import
} finally {
    [gc]::Collect()
    Start-Sleep -Seconds 1
    reg unload "HKLM\$Mount" | Out-Null
}
