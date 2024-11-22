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
# system_boot.ps1
#

# Define the path to the zpool.cache file
$zpoolCachePath = "C:\Windows\System32\Drivers\zpool.cache"

# Check if the zpool.cache file exists
if (Test-Path $zpoolCachePath) {
    Write-Host "zpool.cache found. Importing ZFS pools..."
    zed_log_msg(LOG_NOTICE, "zpool.cache found. Importing ZFS pools");

    # Import ZFS pools using the zpool command and the cache file
    $zpoolImportCommand = "$env:ZPOOL import -c $zpoolCachePath"
    
    # Execute the command
    $result = Invoke-Expression $zpoolImportCommand
    
    # Optionally, log the result or handle any output
    Write-Host "ZFS Pools Imported: $result"
	zed_notify("ZFS Pools Imported", "path");
} else {
    Write-Host "zpool.cache not found. No pools imported."
}
