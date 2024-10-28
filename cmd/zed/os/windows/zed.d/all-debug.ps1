#!/bin/powershell
# shellcheck disable=SC2154
#
# Log all environment variables to ZED_DEBUG_LOG.
#
# This can be a useful aid when developing/debugging ZEDLETs since it shows the
# environment variables defined for each zevent.

if (Test-Path "$env:ZED_ZEDLET_DIR\zed.rc") {
    . "$env:ZED_ZEDLET_DIR\zed.rc"
}
if (Test-Path "$env:ZED_ZEDLET_DIR\zed-functions.ps1") {
    . "$env:ZED_ZEDLET_DIR\zed-functions.ps1"
}

#
# all-debug.ps1
#

Write-Host "all-debug.ps1 is running"

$envVar = $env:ZED_ZEDLET_DIR
if ($envVar) {
    Write-Host "ZED_ZEDLET_DIR is set to: $envVar"
} else {
    Write-Host "ZED_ZEDLET_DIR is not set."
}

Show-ToastNotification -Title "POOL: $env:ZEVENT_POOL" -Message "Event $env:ZEVENT_SUBCLASS has occurred."

