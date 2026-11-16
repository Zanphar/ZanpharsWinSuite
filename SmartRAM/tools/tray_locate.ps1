# tray_locate.ps1 — find our tray icon via UIA (by tooltip text), print its rect.
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

function Dump-TrayIcons {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'Shell_TrayWnd')
    $bar = $root.FindFirst(
        [System.Windows.Automation.TreeScope]::Children, $cond)
    if (-not $bar) { Write-Host 'no Shell_TrayWnd'; return @() }

    $btnCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Button)
    $items = $bar.FindAll([System.Windows.Automation.TreeScope]::Descendants, $btnCond)

    $other = @('NotifyIconOverflowWindow', 'SystemTray_Main')
    $hits = @()
    foreach ($it in $items) {
        $n = $it.Current.Name
        if ($n -match 'RAM') {
            $r = $it.Current.BoundingRectangle
            Write-Host ("FOUND '{0}' at X={1} Y={2} W={3} H={4}" -f $n, $r.X, $r.Y, $r.Width, $r.Height)
            $hits += $r
        }
    }
    # overflow window (hidden area)
    $ofwCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, '显示隐藏的图标')
    $chevron = $bar.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $ofwCond)
    Write-Host ("chevron found: " + ($null -ne $chevron))
    return $hits
}

Dump-TrayIcons
