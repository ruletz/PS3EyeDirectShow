#Requires -Version 5.1
<#
    Attaches the PS3EyeVCamKernel driver to the PS3EyeVCam devnode that already
    exists in the machine.

    pnputil only imports a package that is already present, and a root-enumerated
    node created without a driver never gets picked up by a rescan, so the only
    thing that binds it is UpdateDriverForPlugAndPlayDevices with the hardware
    id from the INF.

    Run elevated, from the folder holding PS3EyeVCamKernel.inf.
#>
param(
    [string]$InfPath,
    [string]$HardwareId = 'PS3EyeVCam'
)

$ErrorActionPreference = 'Stop'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'This script must run elevated (driver install).' -ForegroundColor Red
    exit 1
}

if (-not $InfPath) {
    $scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
    $InfPath = Join-Path $scriptDir 'PS3EyeVCamKernel.inf'
}
if (-not (Test-Path $InfPath)) {
    Write-Host "INF not found: $InfPath" -ForegroundColor Red
    Write-Host 'Run this from the folder that contains PS3EyeVCamKernel.inf (the artifacts folder).'
    exit 1
}
$InfPath = (Resolve-Path $InfPath).Path

$src = @'
using System;
using System.Runtime.InteropServices;

public static class PnpInstall {
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UpdateDriverForPlugAndPlayDevicesW(
        IntPtr hwndParent, string hardwareId, string fullInfPath,
        uint installFlags, [MarshalAs(UnmanagedType.Bool)] out bool rebootRequired);

    public const uint INSTALLFLAG_FORCE = 0x00000001;

    public static int Install(string hardwareId, string infPath, out bool reboot) {
        bool rb;
        bool ok = UpdateDriverForPlugAndPlayDevicesW(
            IntPtr.Zero, hardwareId, infPath, INSTALLFLAG_FORCE, out rb);
        reboot = rb;
        return ok ? 0 : Marshal.GetLastWin32Error();
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

Write-Host "INF       : $InfPath"
Write-Host "HardwareId: $HardwareId"

$reboot = $false
$code = [PnpInstall]::Install($HardwareId, $InfPath, [ref]$reboot)

if ($code -ne 0) {
    $msg = try { (New-Object ComponentModel.Win32Exception($code)).Message } catch { '' }
    Write-Host ("UpdateDriverForPlugAndPlayDevices failed: {0} (0x{0:X8}) {1}" -f $code, $msg) -ForegroundColor Red
    exit 1
}

Write-Host 'Driver attached.' -ForegroundColor Green
if ($reboot) { Write-Host 'Windows reports a reboot is required.' -ForegroundColor Yellow }

Start-Sleep -Seconds 2
$node = Get-PnpDevice -InstanceId 'ROOT\PS3EYEVCAM\0000' -ErrorAction SilentlyContinue
if ($node) {
    $svc = (Get-PnpDeviceProperty -InstanceId $node.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
    Write-Host ("Status  : {0}  Problem: {1}" -f $node.Status, $node.Problem)
    Write-Host ("Service : {0}" -f $(if ($svc) { $svc } else { '<none>' }))
}

$count = 0
foreach ($ref in Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses\{e5323777-f976-4f5b-9b55-b94699c46e44}' -ErrorAction SilentlyContinue) {
    if ($ref.PSChildName -match 'ps3eyevcam') { $count++ }
}
Write-Host ("KSCATEGORY_VIDEO_CAMERA interfaces for this device: {0}" -f $count)
