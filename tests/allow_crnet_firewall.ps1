# Lets every crnet_test.exe in the workspace's build directories accept
# inbound TCP and UDP from the local subnet, so Windows Defender Firewall's
# "Allow access" dialog never stops an unattended run.
#
# Run from an elevated PowerShell, again after a new build directory
# appears (rules go by program path, with no wildcards):
#   carrottus\tests\allow_crnet_firewall.ps1           # add or refresh
#   carrottus\tests\allow_crnet_firewall.ps1 -Public   # also on Public networks
#   carrottus\tests\allow_crnet_firewall.ps1 -Remove   # take them all away
#
# It also removes the rules the dialog made for crnet_test.exe itself: a
# Cancel there makes a Block rule, which wins over any Allow rule.

[CmdletBinding()]
param(
    [switch]$Remove,
    [switch]$Public
)

$ErrorActionPreference = 'Stop'

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error 'Run this from an elevated PowerShell (Run as administrator).'
    exit 1
}

$prefix = 'eqdev crnet_test'
# The workspace (carrottus's parent): other repos build their own copy of
# crnet_test.exe when they add carrottus as a subdirectory. Each rule is
# named by the path below it, which no two copies share.
$root   = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

# The dialog's own rules (named after the program, not ours).
$dialogRules = Get-NetFirewallApplicationFilter |
    Where-Object { $_.Program -like '*\crnet_test.exe' } |
    Get-NetFirewallRule |
    Where-Object { $_.DisplayName -notlike "$prefix*" }
foreach ($rule in $dialogRules) {
    "removed the dialog's rule '$($rule.DisplayName)' ($($rule.Action))"
    $rule | Remove-NetFirewallRule
}

Get-NetFirewallRule -DisplayName "$prefix*" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
if ($Remove) {
    "removed every '$prefix' rule"
    exit 0
}

$profiles = @('Private', 'Domain')
if ($Public) { $profiles += 'Public' }

$exes = Get-ChildItem $root -Recurse -Filter crnet_test.exe -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\CMakeFiles\\' }
if (-not $exes) {
    "no crnet_test.exe under $root yet; build one, then run this again"
    exit 0
}

foreach ($exe in $exes) {
    $name = "$prefix " + $exe.FullName.Substring($root.Length + 1)
    foreach ($protocol in 'TCP', 'UDP') {
        New-NetFirewallRule -DisplayName $name -Direction Inbound -Action Allow `
            -Program $exe.FullName -Protocol $protocol -Profile $profiles `
            -RemoteAddress LocalSubnet | Out-Null
    }
    "allowed $($exe.FullName) ($($profiles -join ', '))"
}
