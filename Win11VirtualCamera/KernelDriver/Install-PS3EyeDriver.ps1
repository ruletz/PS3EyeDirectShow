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

# A device node created with SetupDiCreateDeviceInfo is enumerated but nothing
# runs the install for it: Windows leaves it on the null driver with problem 0,
# so pnputil, newdev and a rescan all consider it configured. The Software
# Device API creates a genuinely new devnode, and PnP installs the staged
# package for it exactly as it would for plugged-in hardware.
$SwDeviceSource = @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class PS3EyeSwDevice
{
    [StructLayout(LayoutKind.Sequential)]
    private struct SW_DEVICE_CREATE_INFO
    {
        public int cbSize;
        public IntPtr pszInstanceId;
        public IntPtr pszzHardwareIds;
        public IntPtr pszzCompatibleIds;
        public IntPtr pContainerId;
        public int CapabilityFlags;
        public IntPtr pszDeviceDescription;
        public IntPtr pszDeviceLocation;
        public IntPtr pSecurityDescriptor;
    }

    private delegate void CreateCallback(IntPtr swDevice, int createResult, IntPtr instanceId, IntPtr context);

    [DllImport("swdevice.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int SwDeviceCreate(string enumerator, string parentDeviceInstance,
        ref SW_DEVICE_CREATE_INFO createInfo, int propertyCount, IntPtr properties,
        CreateCallback callback, IntPtr context, out IntPtr swDevice);

    [DllImport("swdevice.dll")]
    private static extern int SwDeviceSetLifetime(IntPtr swDevice, int lifetime);

    [DllImport("swdevice.dll")]
    private static extern void SwDeviceClose(IntPtr swDevice);

    private const int Removable = 0x00000001;
    private const int SilentInstall = 0x00000002;
    private const int DriverRequired = 0x00000004;
    private const int LifetimeParentPresent = 0x00000001;

    private static IntPtr MultiSz(string value)
    {
        return Marshal.StringToHGlobalUni(value + "\0\0");
    }

    public static string Create(string enumerator, string instanceName, string hardwareId,
        string description, out string deviceInstanceId)
    {
        deviceInstanceId = "";
        IntPtr instance = Marshal.StringToHGlobalUni(instanceName);
        IntPtr hardware = MultiSz(hardwareId);
        IntPtr compatible = MultiSz(hardwareId);
        IntPtr desc = Marshal.StringToHGlobalUni(description);
        IntPtr device = IntPtr.Zero;
        ManualResetEvent finished = new ManualResetEvent(false);
        string reported = "";

        CreateCallback callback = delegate(IntPtr handle, int result, IntPtr id, IntPtr context)
        {
            if (result == 0 && id != IntPtr.Zero) reported = Marshal.PtrToStringUni(id);
            finished.Set();
        };

        try
        {
            SW_DEVICE_CREATE_INFO info = new SW_DEVICE_CREATE_INFO();
            info.cbSize = Marshal.SizeOf(typeof(SW_DEVICE_CREATE_INFO));
            info.pszInstanceId = instance;
            info.pszzHardwareIds = hardware;
            info.pszzCompatibleIds = compatible;
            info.CapabilityFlags = Removable | SilentInstall | DriverRequired;
            info.pszDeviceDescription = desc;

            int hr = SwDeviceCreate(enumerator, "HTREE\\ROOT\\0", ref info, 0, IntPtr.Zero,
                callback, IntPtr.Zero, out device);
            if (hr < 0)
                return "SwDeviceCreate failed (0x" + hr.ToString("X8") + ")";
            if (!finished.WaitOne(30000))
                return "SwDeviceCreate did not report the device within 30 seconds";
            GC.KeepAlive(callback);
            if (device == IntPtr.Zero)
                return "SwDeviceCreate returned no handle";

            // Keep the device after this process exits; it is removed by
            // uninstalling rather than by the handle going away.
            SwDeviceSetLifetime(device, LifetimeParentPresent);
            SwDeviceClose(device);
            deviceInstanceId = reported;
            return "";
        }
        finally
        {
            Marshal.FreeHGlobal(instance);
            Marshal.FreeHGlobal(hardware);
            Marshal.FreeHGlobal(compatible);
            Marshal.FreeHGlobal(desc);
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

    if (-not ('PS3EyeSwDevice' -as [type])) {
        Add-Type -TypeDefinition $SwDeviceSource -Language CSharp
    }

    $existing = @(Get-VcamDevices)
    $device = $existing | Select-Object -First 1

    if ($device) {
        Write-Step ("Device already present: {0} ({1})" -f $device.InstanceId, $device.Status)
    } else {
        Write-Step ("Creating software device {0}..." -f $HardwareId)
        $reported = ''
        $result = [PS3EyeSwDevice]::Create('PS3EYE', $InstanceId, $HardwareId, $DeviceDesc, [ref]$reported)
        if ($result -ne '') {
            Write-Step $result 'ERROR'
            return
        }
        Write-Step ("Windows enumerated {0}." -f $reported)
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
