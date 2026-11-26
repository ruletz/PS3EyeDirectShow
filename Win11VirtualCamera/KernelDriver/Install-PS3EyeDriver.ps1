<#
.SYNOPSIS
    Installs the PS3 Eye AVStream camera driver as a software device.

.DESCRIPTION
    1. checks driver signature enforcement (test signing) and warns if the
       driver cannot load
    2. stages PS3EyeVCamKernel.sys/.inf with pnputil
    3. creates the camera device through the Software Device API, which makes
       PnP install the staged driver the same way it would for real hardware
    4. reports whether a driver (and therefore a service) actually got bound

    Nothing here reboots the machine. If test signing has to be turned on, the
    script prints the bcdedit command and stops: enabling it takes effect only
    after a reboot, and the reboot is yours to decide.

.PARAMETER Uninstall
    Removes the device and the driver package.

.PARAMETER InstallDir
    Folder holding PS3EyeVCamKernel.sys/.inf/.cat. Defaults to this script's
    folder.

.PARAMETER Force
    Continue even if test signing is off and the driver is not signed.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Install-PS3EyeDriver.ps1
#>

#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$Quiet,
    [string]$InstallDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

# $PSScriptRoot is empty while parameters are bound if the script was started
# with `powershell -File`, which made the default install dir an empty string.
if ([string]::IsNullOrEmpty($InstallDir)) {
    $InstallDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
}

# Bare hardware id: it has to match the models line in the INF, and a software
# device is matched on exactly this string.
$HardwareId = 'PS3EyeVCam'
$InstanceId = 'PS3EyeVCam'
$DeviceDesc = 'PS3 Eye Camera'
$DriverName = 'PS3EyeVCamKernel'

$script:ExitCode = 0

function Write-Step {
    param([string]$Message, [string]$Level = 'INFO')
    $line = '[{0}] {1,-5} {2}' -f (Get-Date -Format 'HH:mm:ss'), $Level, $Message
    if (-not $Quiet) {
        switch ($Level) {
            'ERROR' { Write-Host $line -ForegroundColor Red }
            'WARN'  { Write-Host $line -ForegroundColor Yellow }
            'OK'    { Write-Host $line -ForegroundColor Green }
            default { Write-Host $line }
        }
    }
    if ($Level -eq 'ERROR') { $script:ExitCode = 1 }
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# --------------------------------------------------------------- helpers ----

function Test-TestSigning {
    $output = & bcdedit.exe /enum '{current}' 2>&1
    foreach ($line in $output) {
        if ($line -match 'testsigning\s+(\w+)') {
            return ($matches[1] -eq 'Yes')
        }
    }
    return $false
}

function Get-DriverFiles {
    $files = @{}
    foreach ($extension in 'sys', 'inf', 'cat') {
        $path = Join-Path $InstallDir "$DriverName.$extension"
        if (Test-Path $path) { $files[$extension] = $path }
    }
    return $files
}

function Get-VcamDevices {
    @(Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like "*$InstanceId*" })
}

function Get-VcamService([string] $DeviceInstanceId) {
    try {
        [string] (Get-PnpDeviceProperty -InstanceId $DeviceInstanceId -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data
    } catch {
        ''
    }
}

# ------------------------------------------------------- software device ----

# The Software Device API would be the tidy way to do this, but swdevice.dll is
# not present on every Windows 10 build, so this follows devcon instead: create
# a root node and let newdev install the staged package on it.
$RootDeviceSource = @'
using System;
using System.Runtime.InteropServices;

public static class PS3EyeRootDevice
{
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA
    {
        public int cbSize;
        public Guid ClassGuid;
        public int DevInst;
        public IntPtr Reserved;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid classGuid, IntPtr hwndParent);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetupDiCreateDeviceInfo(IntPtr deviceInfoSet, string deviceName,
        ref Guid classGuid, string deviceDescription, IntPtr hwndParent, int creationFlags,
        ref SP_DEVINFO_DATA deviceInfoData);

    // CharSet.Unicode is required: without it this binds to the ANSI export,
    // which rewrites the UTF-16 MULTI_SZ as one string per character. The node
    // then carries hardware id "P";"S";"3";... and newdev reports
    // "Unable to find any matching devices" (SPAPI_E_NO_SUCH_DEVINST,
    // 0xE000020B), so the driver never binds while the device still shows as OK.
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool SetupDiSetDeviceRegistryProperty(IntPtr deviceInfoSet,
        ref SP_DEVINFO_DATA deviceInfoData, int property, byte[] propertyBuffer, int propertyBufferSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiCallClassInstaller(int installFunction, IntPtr deviceInfoSet,
        ref SP_DEVINFO_DATA deviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool UpdateDriverForPlugAndPlayDevices(IntPtr hwndParent, string hardwareId,
        string fullInfPath, int installFlags, out bool rebootRequired);

    private const int DIF_REGISTERDEVICE = 0x00000019;
    private const int SPDRP_HARDWAREID = 0x00000001;
    private const int DICD_GENERATE_ID = 0x00000001;
    private const int INSTALLFLAG_FORCE = 0x00000001;

    // devcon's recipe: a bare device id with DICD_GENERATE_ID, then the same
    // string as the hardware id so the models line in the INF matches it, then
    // newdev to install the package on the node that now exists.
    public static string Create(string hardwareId, string deviceDescription, string infPath)
    {
        Guid mediaClass = new Guid("4d36e96c-e325-11ce-bfc1-08002be10318");
        IntPtr set = SetupDiCreateDeviceInfoList(ref mediaClass, IntPtr.Zero);
        if (set == new IntPtr(-1))
            return "SetupDiCreateDeviceInfoList failed (" + Marshal.GetLastWin32Error() + ")";

        try
        {
            SP_DEVINFO_DATA info = new SP_DEVINFO_DATA();
            info.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));

            if (!SetupDiCreateDeviceInfo(set, hardwareId, ref mediaClass, deviceDescription,
                    IntPtr.Zero, DICD_GENERATE_ID, ref info))
                return "SetupDiCreateDeviceInfo failed (" + Marshal.GetLastWin32Error() + ")";

            string multiSz = hardwareId + "\0\0";
            byte[] buffer = System.Text.Encoding.Unicode.GetBytes(multiSz);
            if (!SetupDiSetDeviceRegistryProperty(set, ref info, SPDRP_HARDWAREID, buffer, buffer.Length))
                return "SetupDiSetDeviceRegistryProperty failed (" + Marshal.GetLastWin32Error() + ")";

            if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref info))
                return "SetupDiCallClassInstaller failed (" + Marshal.GetLastWin32Error() + ")";

            bool reboot = false;
            if (!UpdateDriverForPlugAndPlayDevices(IntPtr.Zero, hardwareId, infPath,
                    INSTALLFLAG_FORCE, out reboot))
                return "UpdateDriverForPlugAndPlayDevices failed (" + Marshal.GetLastWin32Error() + ")";

            return reboot ? "REBOOT" : "";
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
    }
}
'@

# ------------------------------------------------------------------ steps ---

function Install-Driver {
    $files = Get-DriverFiles
    foreach ($required in 'sys', 'inf') {
        if (-not $files.ContainsKey($required)) {
            Write-Step ("Missing {0}\{1}.{2}. Build the driver project first (WDK required)." -f $InstallDir, $DriverName, $required) 'ERROR'
            return
        }
    }

    $inf = $files['inf']
    $sys = $files['sys']
    $cat = $files['cat']

    if (-not (Test-TestSigning) -and -not (Test-Path $cat) -and -not $Force) {
        Write-Step 'The driver is not signed and test signing is off, so Windows will refuse to load it.' 'WARN'
        Write-Step 'Turn it on (elevated), then reboot yourself:' 'WARN'
        Write-Step '    bcdedit /set testsigning on' 'WARN'
        Write-Step 'For a signed driver, just run this script again. Use -Force to try anyway (no reboot performed).' 'WARN'
        $script:ExitCode = 2
        return
    }

    Write-Step ("Staging driver package: {0}" -f (Split-Path -Leaf $inf))
    & pnputil.exe /add-driver "$inf" /install | ForEach-Object { Write-Step $_ }
    if ($LASTEXITCODE -ne 0) {
        Write-Step ("pnputil failed with exit code {0}." -f $LASTEXITCODE) 'WARN'
    }

    if (-not ('PS3EyeRootDevice' -as [type])) {
        Add-Type -TypeDefinition $RootDeviceSource -Language CSharp
    }

    $existing = @(Get-VcamDevices)
    $device = $existing | Select-Object -First 1

    if ($device) {
        Write-Step ("Device already present: {0} ({1})" -f $device.InstanceId, $device.Status)
    } else {
        Write-Step ("Creating device node {0}..." -f $HardwareId)
        $result = [PS3EyeRootDevice]::Create($HardwareId, $DeviceDesc, $inf)
        if ($result -eq 'REBOOT') {
            Write-Step 'Driver bound; Windows wants a reboot to finish. No reboot was started.' 'WARN'
        } elseif ($result -ne '') {
            Write-Step $result 'ERROR'
            return
        } else {
            Write-Step 'Device node created and driver bound.'
        }
    }

    # PnP installs the staged package for the new node; wait for the driver to
    # register its service, which is the only proof the driver actually loaded.
    $service = ''
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 500
        $device = (Get-VcamDevices) | Select-Object -First 1
        if (-not $device) { continue }
        $service = Get-VcamService $device.InstanceId
        if ($service) { break }
    }

    if (-not $device) {
        Write-Step 'The device does not exist after creation.' 'ERROR'
        return
    }

    if ($service) {
        Write-Step ("Bound: {0} [{1}] via service {2}." -f $device.FriendlyName, $device.Status, $service) 'OK'
    } else {
        Write-Step ("Device {0} is present but no driver is bound to it [{1}]." -f $device.InstanceId, $device.Status) 'ERROR'
        Write-Step 'Windows found no matching driver package. Check the hardware id in the INF models line matches this string exactly.' 'WARN'
        return
    }

    Write-Step ''
    Write-Step 'Next: the capture service feeds the driver:' 'OK'
    Write-Step '    PS3EyeCaptureService.exe --install'
    Write-Step 'Then open any camera app and pick "PS3 Eye Camera".' 'OK'
}

function Get-PublishedDrivers {
    $drivers = @()
    $current = @{}
    foreach ($line in (& pnputil.exe /enum-drivers 2>&1)) {
        if ($line -match '^\s*Published Name\s*:\s*(.+?)\s*$') {
            if ($current.Count -gt 0) { $drivers += [pscustomobject]$current }
            $current = @{ PublishedName = $matches[1] }
        } elseif ($line -match '^\s*Original Name\s*:\s*(.+?)\s*$' -and $current.Count -gt 0) {
            $current.OriginalName = $matches[1]
        }
    }
    if ($current.Count -gt 0) { $drivers += [pscustomobject]$current }
    return $drivers
}

function Uninstall-Driver {
    Write-Step ("Removing device {0}..." -f $HardwareId)
    $instances = @(Get-VcamDevices | Select-Object -ExpandProperty InstanceId)
    if (-not $instances) { Write-Step 'No PS3 Eye device present.' 'WARN' }
    foreach ($id in $instances) {
        & pnputil.exe /remove-device "$id" | ForEach-Object { Write-Step $_ }
    }

    Write-Step 'Removing the driver package...'
    $ours = Get-PublishedDrivers | Where-Object {
        $_.OriginalName -and ($_.OriginalName -like ("{0}.inf" -f $DriverName))
    }
    if (-not $ours) {
        Write-Step 'No PS3 Eye driver package found in the driver store.' 'WARN'
    }
    foreach ($driver in $ours) {
        & pnputil.exe /delete-driver $driver.PublishedName /uninstall | ForEach-Object { Write-Step $_ }
    }

    Write-Step 'Driver removed. The capture service is independent and keeps running.' 'OK'
}

# -------------------------------------------------------------------- run ---

if (-not (Test-IsAdmin)) {
    $parts = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath))
    if ($Uninstall) { $parts += '-Uninstall' }
    if ($Quiet) { $parts += '-Quiet' }
    if ($Force) { $parts += '-Force' }
    $parts += @('-InstallDir', ('"{0}"' -f $InstallDir))

    Write-Step 'Administrator rights are required - asking for elevation...'
    $process = Start-Process -FilePath 'powershell.exe' -ArgumentList ($parts -join ' ') -Verb RunAs -Wait -PassThru
    exit $process.ExitCode
}

try {
    if ($Uninstall) { Uninstall-Driver } else { Install-Driver }
} catch {
    Write-Step ("Unexpected error: {0}" -f $_.Exception.Message) 'ERROR'
    Write-Step ("At: {0}" -f $_.InvocationInfo.PositionMessage) 'ERROR'
}

exit $script:ExitCode
