[CmdletBinding()]
param(
    [ValidateRange(1, 3600)]
    [int]$DurationSeconds = 300,
    [string]$StatePath = 'C:\DroidVM\viogpu-58043\viogpu-visual-stimulus-state.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$state = [ordered]@{
    ProcessId = $PID
    SessionId = (Get-Process -Id $PID).SessionId
    StartedAt = (Get-Date).ToString('o')
    ShownAt = $null
    ClosedAt = $null
    FrameCount = 0
    Error = $null
}

try {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing

    $form = [System.Windows.Forms.Form]::new()
    $form.Text = 'VioGpu Fullscreen Present Stimulus'
    $form.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
    $form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
    $form.Bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $form.TopMost = $true
    $form.ShowInTaskbar = $false

    $colors = @(
        [System.Drawing.Color]::FromArgb(255, 255, 0, 255),
        [System.Drawing.Color]::FromArgb(255, 0, 255, 0),
        [System.Drawing.Color]::FromArgb(255, 0, 128, 255),
        [System.Drawing.Color]::FromArgb(255, 255, 255, 0)
    )
    $script:frame = 0
    $timer = [System.Windows.Forms.Timer]::new()
    $timer.Interval = 250
    $timer.Add_Tick({
        $form.BackColor = $colors[$script:frame % $colors.Count]
        $form.Refresh()
        ++$script:frame
        $state.FrameCount = $script:frame
        if ($script:frame -ge (4 * $DurationSeconds)) {
            $timer.Stop()
            $form.Close()
        }
    })
    $form.Add_Shown({
        $state.ShownAt = (Get-Date).ToString('o')
        $state | ConvertTo-Json | Set-Content -LiteralPath $StatePath -Encoding UTF8
        $form.Activate()
        $timer.Start()
    })
    [System.Windows.Forms.Application]::Run($form)
    $state.ClosedAt = (Get-Date).ToString('o')
}
catch {
    $state.Error = $_ | Out-String
}
finally {
    if ($null -ne (Get-Variable -Name timer -ErrorAction SilentlyContinue)) {
        $timer.Dispose()
    }
    if ($null -ne (Get-Variable -Name form -ErrorAction SilentlyContinue)) {
        $form.Dispose()
    }
    $state | ConvertTo-Json | Set-Content -LiteralPath $StatePath -Encoding UTF8
}
