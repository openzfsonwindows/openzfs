# zed-functions.ps1
#
# 2024 Jorgen Lundman <lundman@lundman.net>
#

# This one doesn't need any extra PS modules, but is
# ugly.
function Show-PlainToastNotification {
    param (
        [string]$Title = "ZED Notification",
        [string]$Message = "Default message"
    )

    # Load the required assemblies
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing

    # Create a form to act as a notification
    $form = New-Object System.Windows.Forms.Form
    $form.StartPosition = 'Manual'
    $form.Location = New-Object System.Drawing.Point(0, 0)
    $form.Size = New-Object System.Drawing.Size(300, 100)
    $form.FormBorderStyle = 'None'
    $form.BackColor = [System.Drawing.Color]::White
    $form.TopMost = $true
    $form.ShowInTaskbar = $false
    $form.Opacity = 0.9

    # Create a label for the title
    $titleLabel = New-Object System.Windows.Forms.Label
    $titleLabel.Text = $Title
    $titleLabel.Font = New-Object System.Drawing.Font("Arial", 14, [System.Drawing.FontStyle]::Bold)
    $titleLabel.AutoSize = $true
    $titleLabel.Location = New-Object System.Drawing.Point(10, 10)

    # Create a label for the message
    $messageLabel = New-Object System.Windows.Forms.Label
    $messageLabel.Text = $Message
    $messageLabel.AutoSize = $true
    $messageLabel.Location = New-Object System.Drawing.Point(10, 40)

    # Add the labels to the form
    $form.Controls.Add($titleLabel)
    $form.Controls.Add($messageLabel)

    # Show the form
    $form.Show()

    # Timer to close the notification after 5 seconds
    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 5000 # milliseconds
    $timer.Add_Tick({
        $form.Close()
        $timer.Stop()
    })
    $timer.Start()

    # Run the form's message loop
    [System.Windows.Forms.Application]::Run($form)
}

# For real notifications to work, run in powershell (Admin)
# "Install-Module -Name BurntToast -Force -AllowClobber"
# Failing that, fall back to a regular MessageBox
function Show-ToastNotification {
    param (
        [string]$Title = "Notification",
        [string]$Message = "This is a test notification."
    )

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

# Function to show a message box
function Show-MessageBox {
    param (
        [string]$Message,
        [string]$Title
    )
    
    # Load the necessary assembly
    Add-Type -AssemblyName System.Windows.Forms
    
    # Display the message box
    [System.Windows.Forms.MessageBox]::Show($Message, $Title)
}


