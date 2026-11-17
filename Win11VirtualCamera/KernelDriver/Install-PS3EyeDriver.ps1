<#
.SYNOPSIS
    Installs the PS3 Eye AVStream camera driver (root-enumerated device).

.DESCRIPTION
    1. checks driver signature enforcement (test signing) and warns if the
       driver cannot load
    2. stages PS3EyeVCamKernel.sys/.inf with pnputil
    3. creates the ROOT\PS3EyeVCam device node and binds the driver to it
    4. verifies that the device exists

    Nothing here reboots the machine. If test signing has to be turned on, the
    script prints the bcdedit command and stops: enabling it takes effect only
    after a reboot, and the reboot is yours to decide.

.PARAMETER Uninstall
    Removes the device node and the driver package.

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

$HardwareId = 'ROOT\PS3EyeVCam'
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

# ---------------------------------------------------- root device creation ---

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

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiSetDeviceRegistryProperty(IntPtr deviceInfoSet,
        ref SP_DEVINFO_DATA deviceInfoData, int property, byte[] propertyBuffer, int propertyBufferSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiCallClassInstaller(int installFunction, IntPtr deviceInfoSet,
        ref SP_DEVINFO_DATA deviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    private const int DIF_REGISTERDEVICE = 0x00000019;
    private const int SPDRP_HARDWAREID = 0x00000001;
    private const int DICD_GENERATE_ID = 0x00000001;


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

            // DICD_GENERATE_ID takes the device id without the enumerator prefix;
            // handing it the full instance path fails with 0xE0000205. The node
            // ends up as ROOT\PS3EyeVCam\0000 with ROOT\PS3EyeVCam as its
            // hardware id, which is what the INF matches on.
            string instanceName = hardwareId.Substring(hardwareId.LastIndexOf('\\') + 1);
            if (!SetupDiCreateDeviceInfo(set, instanceName, ref mediaClass, deviceDescription,
                    IntPtr.Zero, DICD_GENERATE_ID, ref info))
                return "SetupDiCreateDeviceInfo failed (" + Marshal.GetLastWin32Error() + ")";

            string multiSz = hardwareId + "\0\0";
            byte[] buffer = System.Text.Encoding.Unicode.GetBytes(multiSz);
            if (!SetupDiSetDeviceRegistryProperty(set, ref info, SPDRP_HARDWAREID, buffer, buffer.Length))
                return "SetupDiSetDeviceRegistryProperty failed (" + Marshal.GetLastWin32Error() + ")";

            if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref info))
                return "SetupDiCallClassInstaller failed (" + Marshal.GetLastWin32Error() + ")";

            // No binding here: the package is already staged, so PnP installs it
            // as soon as the node appears. UpdateDriverForPlugAndPlayDevices does
            // the same thing further down the road and fails with ERROR_NO_SUCH_DEVINST
            // while the new node is not in the hardware tree yet.
            return "";
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
    }
}
'@

# ------------------------------------------------------------- root rescan ---

# Rescanning the root enumerator is what Device Manager's "Scan for hardware
# changes" does; it is the only way to make PnP look at a node it has already
# marked as started.
$RescanSource = @'
using System;
using System.Runtime.InteropServices;

public static class PS3EyeRescan
{
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    public static extern int CM_Locate_DevNode(out uint devInst, string deviceId, uint flags);

    [DllImport("cfgmgr32.dll")]
    public static extern int CM_Reenumerate_DevNode(uint devInst, uint flags);
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

    Write-Step ("Creating device node {0}..." -f $HardwareId)
    $result = [PS3EyeRootDevice]::Create($HardwareId, $DeviceDesc, $inf)

    if ($result -ne '') {
        Write-Step $result 'ERROR'
        return
    }
    Write-Step 'Device node created; binding the staged driver.'

    function Get-VcamDevice {
        Get-PnpDevice -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -like ("{0}*" -f $HardwareId) } | Select-Object -First 1
    }

    function Get-VcamService([string] $InstanceId) {
        try {
            [string] (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName DEVPKEY_Device_Service -ErrorAction Stop).Data
        } catch {
            ''
        }
    }

    $device = $null
    for ($i = 0; $i -lt 10 -and -not $device; $i++) {
        Start-Sleep -Milliseconds 500
        $device = Get-VcamDevice
    }
    if (-not $device) {
        Write-Step 'The device node did not appear.' 'ERROR'
        $script:ExitCode = 1
        return
    }

    # A root node created through SetupAPI is enumerated, but nothing runs the
    # install step for it: with no driver chosen Windows marks it started on the
    # null driver with problem 0, so it looks healthy and pnputil has nothing to
    # act on. Disabling and re-enabling makes PnP run driver selection against
    # the staged package.
    if (-not (Get-VcamService $device.InstanceId)) {
        Write-Step 'No driver bound yet; cycling the device to trigger the install.'
        try {
            Disable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction Stop
        } catch {
            Write-Step ("Disable-PnpDevice failed: {0}" -f $_.Exception.Message) 'WARN'
        }
        Start-Sleep -Seconds 1
        try {
            Enable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction Stop
        } catch {
            Write-Step ("Enable-PnpDevice failed: {0}" -f $_.Exception.Message) 'WARN'
        }
    }

    for ($i = 0; $i -lt 10; $i++) {
        Start-Sleep -Milliseconds 500
        $device = Get-VcamDevice
        if ($device -and (Get-VcamService $device.InstanceId)) { break }
    }

    # Last resort: mark the node for reinstall and rescan the root enumerator.
    if (-not (Get-VcamService $device.InstanceId)) {
        Write-Step 'Still unbound; flagging the node for reinstall and rescanning the root.' 'WARN'
        $enumKey = "HKLM:\SYSTEM\CurrentControlSet\Enum\{0}" -f $device.InstanceId
        $flags = 0
        try { $flags = [int] (Get-ItemProperty -Path $enumKey -Name ConfigFlags -ErrorAction Stop).ConfigFlags } catch { }
        try {
            Set-ItemProperty -Path $enumKey -Name ConfigFlags -Value ($flags -bor 0x20) -Type DWord -ErrorAction Stop
        } catch {
            Write-Step ("Could not set ConfigFlags: {0}" -f $_.Exception.Message) 'WARN'
        }
        if (-not ('PS3EyeRescan' -as [type])) {
            Add-Type -TypeDefinition $RescanSource -Language CSharp
        }
        $dn = 0
        if ([PS3EyeRescan]::CM_Locate_DevNode([ref]$dn, 'HTREE\ROOT\0', 0) -eq 0) {
            $cr = [PS3EyeRescan]::CM_Reenumerate_DevNode($dn, 1)
            Write-Step ("Root rescan returned {0}." -f $cr)
        }
        Start-Sleep -Seconds 3
        $device = Get-VcamDevice
    }

    $service = if ($device) { Get-VcamService $device.InstanceId } else { '' }
    if ($service) {
        Write-Step ("Bound to {0} [{1}] via service {2}." -f $device.FriendlyName, $device.Status, $service) 'OK'
    } else {
        Write-Step 'The node exists but Windows bound no driver to it.' 'ERROR'
        $script:ExitCode = 1
    }

    try {
        if ($device) {
            Write-Step ("Present: {0} [{1}] ({2})" -f $device.FriendlyName, $device.Status, $device.InstanceId) 'OK'
        } else {
            Write-Step 'The device node is registered but not present yet; a reboot finishes the binding.' 'WARN'
        }
    } catch {
        Write-Step ("Could not query the device: {0}" -f $_.Exception.Message) 'WARN'
    }

    Write-Step ''
    Write-Step 'Next: make sure PS3EyeCaptureService is running (it feeds the driver):' 'OK'
    Write-Step '    "%ProgramFiles%\PS3 Eye\PS3EyeVCamRegistrar.exe" status'
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
    # The instance id carries a generated suffix, so look it up rather than guess.
    $instances = @(Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like ("{0}*" -f $HardwareId) } |
        Select-Object -ExpandProperty InstanceId)
    if (-not $instances) { Write-Step 'No PS3 Eye device node present.' 'WARN' }
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
