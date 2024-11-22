# zed-functions.ps1
#
# 2024 Jorgen Lundman <lundman@lundman.net>
#

# zed_log_msg (msg, ...)
#
# Write all argument strings to the system log.
#
# Globals
#   ZED_SYSLOG_PRIORITY
#   ZED_SYSLOG_TAG
#
# Return
#   nothing
#
function zed_log_msg {
    param (
        [string]$priority,
        [string]$tag,
        [string]$message
    )

    # Check if the event source exists, create it if not
    $eventSource = "OpenZFS_zed"
    if (-not (EventLog SourceExists $eventSource)) {
        New-EventLog -LogName Application -Source $eventSource
    }

    # Create the log message with priority and tag
    $logMessage = "[$priority] [$tag] $message"

    # Log the message to the Windows Event Log
    Write-EventLog -LogName Application -Source $eventSource -EntryType Information -EventId 1001 -Message $logMessage
}

function zed_log_err {
    param (
        [string]$priority,
        [string]$tag,
        [string]$message
    )

    # Create the log entry format
    $logEntry = "$(Get-Date) [$priority] [$tag] $message"
    
    # Check if the event source exists, create it if not
    $eventSource = "ZED"
    if (-not (EventLog SourceExists $eventSource)) {
        New-EventLog -LogName Application -Source $eventSource
    }

    # Log to the Windows Event Log as an Error (higher priority)
    Write-EventLog -LogName Application -Source $eventSource -EntryType Error -EventId 1002 -Message $logEntry
}


# For real notifications to work, run in powershell (Admin)
# "Install-Module -Name BurntToast -Force -AllowClobber"
# Failing that, fall back to a regular MessageBox
function zed_notify_burnttoast()
{
    param (
        [string]$Title = "Notification",
        [string]$Message = "This is a test notification."
    )

	if (-not $env:ZED_BURNTTOAST_NOTIFICATIONS) {
		return 2
	}

    # Check if the BurntToast module is available
    if (Get-Module -ListAvailable -Name BurntToast) {
        try {
            # Import the module
            Import-Module BurntToast -ErrorAction Stop
            
            # Create a BurntToast notification
            New-BurntToastNotification -Text $Title, $Message
        }
        catch {
            # If there's an error, fall back to a basic notification
            Write-Host "BurntToast failed to create a notification: $_"
            Show-MessageBox "$Message" "$Title"
        }
    }
    else {
        # Fallback if BurntToast is not installed
        Write-Host "BurntToast module is not installed. Falling back to basic notification."
        Show-MessageBox "$Message" "$Title"
    }
}

function zed_notify_toast
{
    param (
        [string]$subject,
        [string]$pathname
    )

	if (-not $env:ZED_TOAST_NOTIFICATIONS) {
		return 2
	}

    # Check if the 'subject' (title) is provided, otherwise use a default
    if (-not $subject) {
        $subject = "OpenZFS Notification"
    }

    # Create the message text from the pathname or fallback to the subject
    $message = if ($pathname) { "Details: $pathname" } else { "No additional details provided" }

    # Create the Toast notification
    $toastXml = @"
<toast>
    <visual>
        <binding template="ToastGeneric">
            <text>$subject</text>
            <text>$message</text>
        </binding>
    </visual>
</toast>
"@

    # Create the Toast notification
    $toast = [Windows.UI.Notifications.ToastNotification]::new([Windows.Data.Xml.Dom.XmlDocument]::new())
    $toast.XmlDocument.LoadXml($toastXml)

    # Display the Toast
    [Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier("ZFSNotificationApp").Show($toast)

    # Return success (0)
    return 0
}

# Function to show a message box
function zed_notify_messagebox
{
    param (
        [string]$Message,
        [string]$Title
    )
    
    # Load the necessary assembly
    Add-Type -AssemblyName System.Windows.Forms
    
    # Display the message box
    [System.Windows.Forms.MessageBox]::Show($Message, $Title)
}

zed_notify()
{
    local subject="$1"
    local pathname="$2"
    local num_success=0
    local num_failure=0

    # Call zed_notify_toast (Windows-specific toast notification)
    zed_notify_toast "$subject" "$pathname"
    rv=$?
    [ "$rv" -eq 0 ] && num_success=$((num_success + 1))
    [ "$rv" -eq 1 ] && num_failure=$((num_failure + 1))

	zed_notify_burnttoast "$subject" "$pathname"
    rv=$?
    [ "$rv" -eq 0 ] && num_success=$((num_success + 1))
    [ "$rv" -eq 1 ] && num_failure=$((num_failure + 1))

    # Additional notification methods could go here (email, pushbullet, etc.)

    # If we had any success, return success code
    if [ "$num_success" -gt 0 ]; then
        return 0
    fi

    # If we had any failure, return failure code
    if [ "$num_failure" -gt 0 ]; then
        return 1
    fi

    # If no methods were configured, return 2
    return 2
}


# zed_guid_to_pool (guid)
#
# Convert a pool GUID into its pool name (like "tank")
# Arguments
#   guid: pool GUID (decimal or hex)
#
# Return
#   Pool name
function zed_guid_to_pool
{
    param (
        [Parameter(Mandatory=$true)]
        [string]$guid
    )

    if (-not $guid) {
        return # Return nothing if no GUID is provided
    }

    # Check if the GUID is in hex format (starts with "0x")
    if ($guid.StartsWith("0x", [StringComparison]::OrdinalIgnoreCase)) {
        # Strip the "0x" prefix and convert it to a decimal number
        $guid = [Convert]::ToUInt64($guid.Substring(2), 16)
    }
    
    # Run the zpool command to get the pool name for the GUID
    try {
        $poolName = & $env:ZPOOL get -H -ovalue,name guid | Where-Object { $_ -match "^$guid\s+(\S+)$" } | ForEach-Object { $_ -replace "^$guid\s+", "" }

        if ($poolName) {
            return $poolName
        } else {
            Write-Host "Pool not found for GUID: $guid"
            return ""
        }
    }
    catch {
        Write-Error "Error querying zpool: $_"
        return ""
    }
}
