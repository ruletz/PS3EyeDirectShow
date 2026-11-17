<#
.SYNOPSIS
    Publishes the PS3 Eye's existing capture device to Media Foundation.

.DESCRIPTION
    The camera's capture device (USB\VID_1415&PID_2000&MI_01) works in
    DirectShow but only registers KSCATEGORY_CAPTURE. Media Foundation - and so
    Chromium, Brave, and the Windows camera stack - enumerate
    KSCATEGORY_VIDEO_CAPTURE and KSCATEGORY_VIDEO_CAMERA, which nothing
    publishes on this machine.

    PS3EyeCamInterface.sys is an upper filter for that device. It forwards every
    IRP untouched and registers those two interfaces on the device, so the
    existing driver keeps streaming and every app that asks Media Foundation for
    a camera sees it.

    Nothing here replaces the camera's own driver, touches the USB pipes or the
    capture service. Uninstalling removes the filter and the device goes back to
    the DirectShow-only state.

.PARAMETER Uninstall
    Removes the filter from the device and deletes the service and file.

.PARAMETER InstallDir
    Folder holding PS3EyeCamInterface.sys. Defaults to this script's folder.
#>

#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Uninstall,
    [string]$InstallDir
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($InstallDir)) {
    $InstallDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
}

$ServiceName = 'PS3EyeCamInterface'
$DriverFile = "$env:SystemRoot\System32\drivers\$ServiceName.sys"
$CameraInstance = 'USB\VID_1415&PID_2000&MI_01'

$script:ExitCode = 0

function Write-Step {
    param([string]$Message, [string]$Level = 'INFO')
    $line = '[{0}] {1,-5} {2}' -f (Get-Date -Format 'HH:mm:ss'), $Level, $Message
    switch ($Level) {
        'ERROR' { Write-Host $line -ForegroundColor Red }
        'WARN'  { Write-Host $line -ForegroundColor Yellow }
        'OK'    { Write-Host $line -ForegroundColor Green }
        default { Write-Host $line }
    }
    if ($Level -eq 'ERROR') { $script:ExitCode = 1 }
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-CameraKey {
    $root = "HKLM:\SYSTEM\CurrentControlSet\Enum\$CameraInstance"
    if (-not (Test-Path $root)) { return $null }
    $instances = @(Get-ChildItem $root -ErrorAction SilentlyContinue)
    if ($instances.Count -eq 0) { return $null }
    return $instances[0].PSPath
}

function Get-FilterList([string] $Key) {
    $value = (Get-ItemProperty -Path $Key -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
    if ($null -eq $value) { return @() }
    return @($value)
}

function Set-FilterList([string] $Key, [string[]] $Filters) {
    if ($Filters.Count -eq 0) {
        Remove-ItemProperty -Path $Key -Name UpperFilters -ErrorAction SilentlyContinue
    } else {
        New-ItemProperty -Path $Key -Name UpperFilters -PropertyType MultiString -Value $Filters -Force | Out-Null
    }
}

function Get-CameraInstanceId {
    # The device *id* is not an instance id: only the instance carries the
    # trailing suffix, and only that form works with pnputil.
    $device = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like ("{0}*" -f $CameraInstance) } |
        Select-Object -First 1
    if (-not $device) { return $null }
    return $device.InstanceId
}

function Restart-Camera {
    # Windows only loads a filter when the device stack is rebuilt.
    $instance = Get-CameraInstanceId
    if (-not $instance) {
        Write-Step 'The camera is not plugged in; the filter loads the next time it is.' 'WARN'
        return $false
    }
    Write-Step ("Restarting {0}..." -f $instance)
    & pnputil.exe /restart-device "$instance" 2>&1 | ForEach-Object { Write-Step ([string]$_) }
    if ($LASTEXITCODE -ne 0) {
        Write-Step 'pnputil could not restart the device; unplug and replug the camera.' 'WARN'
        return $false
    }
    return $true
}

function Test-CameraInterface {
    $script = @'
$src = @"
using System;
using System.Runtime.InteropServices;
public static class ProbeOne {
  [StructLayout(LayoutKind.Sequential)] struct D { public int cbSize; public Guid g; public int f; public IntPtr r; }
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr e, IntPtr h, int f);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, int i, ref D data);
  [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
  public static int Count(string text) {
    Guid g = new Guid(text); int n = 0;
    IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, 0x12);
    if (set == new IntPtr(-1)) return -1;
    D data = new D(); data.cbSize = Marshal.SizeOf(typeof(D));
    while (SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, n, ref data)) n++;
    SetupDiDestroyDeviceInfoList(set); return n;
  }
}
"@
Add-Type -TypeDefinition $src -Language CSharp
[ProbeOne]::Count('E5323777-F976-4F5B-9B55-B94699C46E44')
'@
    $path = Join-Path $env:TEMP 'ps3eye-probe.ps1'
    Set-Content -Path $path -Value $script -Encoding UTF8
    try {
        $count = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $path
        return [int] ($count | Select-Object -Last 1)
    } finally {
        Remove-Item $path -ErrorAction SilentlyContinue
    }
}

# --------------------------------------------------------------------- main

if (-not (Test-IsAdmin)) {
    $parts = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath))
    if ($Uninstall) { $parts += '-Uninstall' }
    $parts += @('-InstallDir', ('"{0}"' -f $InstallDir))
    Write-Step 'Administrator rights are required - asking for elevation...'
    $process = Start-Process -FilePath 'powershell.exe' -ArgumentList ($parts -join ' ') -Verb RunAs -Wait -PassThru
    exit $process.ExitCode
}

try {
    $key = Get-CameraKey
    if (-not $key) {
        Write-Step "The camera device ($CameraInstance) has no registry key. Plug it in and run this again." 'ERROR'
        exit 1
    }
    if (-not (Get-CameraInstanceId)) {
        Write-Step "The camera device ($CameraInstance) is not plugged in." 'ERROR'
        exit 1
    }

    if ($Uninstall) {
        Write-Step "Removing $ServiceName from the camera's filters..."
        Set-FilterList $key (@(Get-FilterList $key) | Where-Object { $_ -ne $ServiceName })
        Restart-Camera
        & sc.exe stop $ServiceName | Out-Null
        & sc.exe delete $ServiceName | Out-Null
        if (Test-Path $DriverFile) { Remove-Item $DriverFile -Force }
        Write-Step 'Filter removed. The camera is DirectShow-only again.' 'OK'
        exit $script:ExitCode
    }

    $source = Join-Path $InstallDir "$ServiceName.sys"
    if (-not (Test-Path $source)) {
        Write-Step "Missing $ServiceName.sys in $InstallDir" 'ERROR'
        exit 1
    }

    Write-Step "Copying $ServiceName.sys to the drivers folder..."
    Copy-Item $source $DriverFile -Force

    Write-Step 'Registering the filter service...'
    $existing = & sc.exe query $ServiceName 2>&1
    if ($LASTEXITCODE -eq 0) {
        & sc.exe config $ServiceName binPath= "System32\drivers\$ServiceName.sys" type= kernel start= demand | Out-Null
        Write-Step 'Service updated.'
    } else {
        $output = & sc.exe create $ServiceName binPath= "System32\drivers\$ServiceName.sys" type= kernel start= demand 2>&1
        $output | ForEach-Object { Write-Step ([string]$_) }
        if ($LASTEXITCODE -ne 0) {
            Write-Step 'sc create failed.' 'ERROR'
            exit 1
        }
    }

    Write-Step "Attaching the filter to $CameraInstance..."
    $filters = @(Get-FilterList $key)
    if ($filters -notcontains $ServiceName) {
        Set-FilterList $key ($filters + $ServiceName)
    }
    Write-Step ("UpperFilters = {0}" -f ((@(Get-FilterList $key)) -join ', '))

    Restart-Camera
    Start-Sleep -Seconds 2

    $count = Test-CameraInterface
    Write-Step ("KSCATEGORY_VIDEO_CAMERA interfaces now: {0}" -f $count)
    if ($count -gt 0) {
        Write-Step 'The camera is published to Media Foundation. Open Brave and check its camera list.' 'OK'
    } else {
        Write-Step 'No camera interface yet. If the driver could not load (test signing off) the device will show an error.' 'WARN'
    }
} catch {
    Write-Step ("Unexpected error: {0}" -f $_.Exception.Message) 'ERROR'
    Write-Step ("At: {0}" -f $_.InvocationInfo.PositionMessage) 'ERROR'
}

exit $script:ExitCode
