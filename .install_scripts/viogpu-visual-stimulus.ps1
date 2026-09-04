$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = [System.Windows.Forms.Form]::new()
$form.Text = 'VioGpu Present Stimulus'
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::CenterScreen
$form.ClientSize = [System.Drawing.Size]::new(800, 600)
$form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::FixedSingle
$form.TopMost = $true

$colors = @(
    [System.Drawing.Color]::FromArgb(255, 32, 64, 192),
    [System.Drawing.Color]::FromArgb(255, 192, 48, 32),
    [System.Drawing.Color]::FromArgb(255, 32, 176, 80),
    [System.Drawing.Color]::FromArgb(255, 224, 192, 32)
)
$script:frame = 0
$timer = [System.Windows.Forms.Timer]::new()
$timer.Interval = 50
$timer.Add_Tick({
    $form.BackColor = $colors[$script:frame % $colors.Count]
    $form.Refresh()
    ++$script:frame
    if ($script:frame -ge 160) {
        $timer.Stop()
        $form.Close()
    }
})
$form.Add_Shown({ $timer.Start() })
[System.Windows.Forms.Application]::Run($form)
$timer.Dispose()
$form.Dispose()
