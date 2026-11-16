<#
.SYNOPSIS
    Installs the PS3 Eye as a native Windows 11 camera, in one pass.

.DESCRIPTION
    Does, in order:
      1. (optional) installs the PS3 Eye Universal WinUSB driver + 64-bit
         DirectShow filter from the bundled MSI (or registers the filter DLL
         that is sitting next to this script)
      2. copies the capture service, the Media Foundation source and the
         registrar into Program Files
      3. installs and starts PS3EyeCaptureService (owns the camera over WinUSB)
      4. registers PS3EyeVCamSource.dll (HKLM, COM in-proc server)
      5. creates the virtual camera via PS3EyeVCamRegistrar.exe install
      6. verifies that Media Foundation enumerates the camera

    Everything is logged to %ProgramData%\PS3 Eye\install.log.

.PARAMETER Uninstall
    Reverses all of the above.

.PARAMETER Quiet
    Only write to the log file, no console output.

.PARAMETER InPlace
    The binaries already live in -InstallDir (used by the Inno Setup package);
    skip the copy step.

.PARAMETER InstallDir
    Target folder. Default: C:\Program Files\PS3 Eye

.PARAMETER DriverMsi
    Explicit path to a PS3 Eye driver MSI. When omitted, the first *.msi found
    next to the script is used, if any.

.PARAMETER SkipDriver
    Do not touch the WinUSB driver or the DirectShow filter. The native camera
    does not need them; the DirectShow filter is only a fallback.

.PARAMETER IgnoreOsCheck
    Proceed even though the virtual camera API needs Windows 11 (22000+).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Install-PS3EyeNative.ps1

.EXAMPLE
    .\Install-PS3EyeNative.ps1 -Uninstall
#>

#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Uninstall,
    [switch]$Quiet,
    [switch]$InPlace,
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'PS3 Eye'),
    [string]$DriverMsi = '',
    [switch]$SkipDriver,
    [switch]$IgnoreOsCheck
)

$ErrorActionPreference = 'Stop'

$script:LogPath = Join-Path $env:ProgramData 'PS3 Eye\install.log'
$script:ExitCode = 0

$ServiceName = 'PS3EyeCaptureService'
$SourceClsid = '{E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}'
$ClsidKey = "HKLM:\SOFTWARE\Classes\CLSID\$SourceClsid\InprocServer32"

# ---------------------------------------------------------------- logging ----

function Initialize-Log {
    $dir = Split-Path -Parent $script:LogPath
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $header = "=== {0} : {1} ===" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'),
        ($(if ($Uninstall) { 'uninstall' } else { 'install' }))
    Add-Content -Path $script:LogPath -Value $header -Encoding UTF8
}

function Write-Log {
    param(
        [string]$Message,
        [ValidateSet('INFO', 'OK', 'WARN', 'ERROR')][string]$Level = 'INFO'
    )

    $line = '[{0}] {1,-5} {2}' -f (Get-Date -Format 'HH:mm:ss'), $Level, $Message
    try { Add-Content -Path $script:LogPath -Value $line -Encoding UTF8 } catch { }

    if ($Quiet) { return }
    switch ($Level) {
        'ERROR' { Write-Host $line -ForegroundColor Red }
        'WARN'  { Write-Host $line -ForegroundColor Yellow }
        'OK'    { Write-Host $line -ForegroundColor Green }
        default { Write-Host $line }
    }
}

function Fail {
    param([string]$Message)
    Write-Log $Message 'ERROR'
    $script:ExitCode = 1
}

# ------------------------------------------------------------- elevation ----

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-Elevated {
    $parts = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"{0}"' -f $PSCommandPath))
    if ($Uninstall) { $parts += '-Uninstall' }
    if ($Quiet) { $parts += '-Quiet' }
    if ($InPlace) { $parts += '-InPlace' }
    if ($SkipDriver) { $parts += '-SkipDriver' }
    if ($IgnoreOsCheck) { $parts += '-IgnoreOsCheck' }
    $parts += @('-InstallDir', ('"{0}"' -f $InstallDir))
    if ($DriverMsi) { $parts += @('-DriverMsi', ('"{0}"' -f $DriverMsi)) }

    Write-Host 'Administrator rights are required - asking for elevation...'
    try {
        $process = Start-Process -FilePath 'powershell.exe' -ArgumentList ($parts -join ' ') -Verb RunAs -Wait -PassThru
        return $process.ExitCode
    } catch {
        Write-Host 'Elevation was declined; nothing was changed.' -ForegroundColor Red
        return 1
    }
}

# ------------------------------------------------------------- processes ----

function Invoke-Exe {
    param(
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [int[]]$OkCodes = @(0)
    )

    Write-Log ("Running: {0} {1}" -f $FilePath, ($Arguments -join ' '))
    $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -Wait -PassThru -NoNewWindow
    if ($OkCodes -contains $process.ExitCode) {
        return $process.ExitCode
    }

    Write-Log ("{0} exited with code {1}" -f (Split-Path -Leaf $FilePath), $process.ExitCode) 'ERROR'
    return $process.ExitCode
}

function Stop-CaptureService {
    & sc.exe stop $ServiceName 2>&1 | Out-Null
    Start-Sleep -Seconds 2
}

function Get-ServiceState {
    param([string]$Name)
    try {
        $svc = Get-Service -Name $Name -ErrorAction Stop
        return $svc.Status
    } catch {
        return 'NotInstalled'
    }
}

# --------------------------------------------------------------- payload ----

$RequiredFiles = @(
    'PS3EyeCaptureService.exe',
    'PS3EyeVCamSource.dll',
    'PS3EyeVCamRegistrar.exe'
)

function Resolve-Payload {
    if ($InPlace) {
        return $InstallDir
    }

    $candidates = @(
        (Join-Path $PSScriptRoot 'payload'),
        $PSScriptRoot,
        (Join-Path $PSScriptRoot '..\bin\x64\Release'),
        (Join-Path $PSScriptRoot '..\..\bin\x64\Release')
    )

    foreach ($candidate in $candidates) {
        if (-not (Test-Path $candidate)) { continue }
        $missing = @($RequiredFiles | Where-Object { -not (Test-Path (Join-Path $candidate $_)) })
        if ($missing.Count -eq 0) {
            return (Resolve-Path $candidate).Path
        }
    }

    Fail 'Could not find the PS3 Eye binaries. Build the solutions first; the'
    Fail ('expected files are: {0}' -f ($RequiredFiles -join ', '))
    return $null
}

function Get-FilterDll {
    param([string]$Directory)
    foreach ($name in @('PS3EyeSourceFilter64.dll', 'PS3EyeSourceFilter.dll')) {
        $path = Join-Path $Directory $name
        if (Test-Path $path) { return $path }
    }
    return $null
}

# ----------------------------------------------------------------- steps ----

function Install-Driver {
    param([string]$Source)

    if ($SkipDriver) {
        Write-Log 'Skipping WinUSB driver and DirectShow filter (-SkipDriver).'
        return
    }

    $msi = $DriverMsi
    if (-not $msi) {
        $msi = Get-ChildItem -Path $Source -Filter '*.msi' -File -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
    }

    if ($msi -and (Test-Path $msi)) {
        Write-Log ("Installing PS3 Eye Universal driver + DirectShow filter: {0}" -f (Split-Path -Leaf $msi))
        # 0 = success, 3010 = success, reboot required
        $code = Invoke-Exe 'msiexec.exe' @('/i', "`"$msi`"", '/qn', '/norestart') -OkCodes @(0, 3010)
        if ($code -in 0, 3010) {
            Write-Log 'Driver and DirectShow filter installed.' 'OK'
        } else {
            Write-Log 'Driver MSI failed. The native camera can still work if the PS3 Eye is already bound to WinUSB.' 'WARN'
        }
        return
    }

    $filter = Get-FilterDll -Directory $InstallDir
    if (-not $filter) {
        $filter = Get-FilterDll -Directory $Source
    }

    if ($filter) {
        Write-Log ("Registering DirectShow filter directly: {0}" -f (Split-Path -Leaf $filter))
        Invoke-Exe (Join-Path $env:SystemRoot 'System32\regsvr32.exe') @('/s', "`"$filter`"") | Out-Null
        Write-Log 'DirectShow filter registered (WinUSB driver assumed to be in place).' 'OK'
        return
    }

    Write-Log 'No driver MSI and no DirectShow filter found; skipping. The camera needs the PS3 Eye bound to WinUSB.' 'WARN'
}

function Install-Service {
    param([string]$Directory)

    $exe = Join-Path $Directory 'PS3EyeCaptureService.exe'
    $code = Invoke-Exe $exe @('--install') -OkCodes @(0)

    if ($code -ne 0) {
        # Already installed is not a failure: make sure it is running.
        if ((Get-ServiceState $ServiceName) -ne 'NotInstalled') {
            Write-Log 'Capture service was already installed; trying to start it.' 'WARN'
            & sc.exe start $ServiceName 2>&1 | Out-Null
            Start-Sleep -Seconds 2
        } else {
            Fail 'Could not install PS3EyeCaptureService.'
            return $false
        }
    }

    $state = Get-ServiceState $ServiceName
    if ($state -eq 'Running') {
        Write-Log 'PS3EyeCaptureService is installed and running.' 'OK'
        return $true
    }

    Write-Log ("Capture service state is '{0}' - frames will be black until it runs." -f $state) 'WARN'
    return $true
}

function Register-Source {
    param([string]$Directory)

    $dll = Join-Path $Directory 'PS3EyeVCamSource.dll'
    Invoke-Exe (Join-Path $env:SystemRoot 'System32\regsvr32.exe') @('/s', "`"$dll`"") | Out-Null

    if (-not (Test-Path $ClsidKey)) {
        Fail "COM registration failed: $ClsidKey is missing (regsvr32 PS3EyeVCamSource.dll)."
        return $false
    }

    Write-Log 'Media Foundation source registered under HKLM.' 'OK'
    return $true
}

function Add-VirtualCamera {
    param([string]$Directory)

    $registrar = Join-Path $Directory 'PS3EyeVCamRegistrar.exe'
    $code = Invoke-Exe $registrar @('install') -OkCodes @(0)
    if ($code -ne 0) {
        Fail 'Failed to create the virtual camera (see the registrar output above).'
        return $false
    }
    return $true
}

function Test-Result {
    param([string]$Directory)

    $registrar = Join-Path $Directory 'PS3EyeVCamRegistrar.exe'
    Write-Log 'Checking that Media Foundation enumerates the camera...'
    $process = Start-Process -FilePath $registrar -ArgumentList 'status' -Wait -PassThru -NoNewWindow

    if ($process.ExitCode -eq 0) {
        Write-Log 'PS3 Eye is visible to Media Foundation apps (Brave, Teams, Camera app).' 'OK'
        return $true
    }

    Write-Log 'The camera is registered but not enumerating yet. Reboot, then run:' 'WARN'
    Write-Log ("  `"{0}`" status" -f $registrar)
    return $false
}

# ----------------------------------------------------------------- main -----

function Invoke-Install {
    $source = Resolve-Payload
    if (-not $source) { return }

    Write-Log ("Source : {0}" -f $source)
    Write-Log ("Target : {0}" -f $InstallDir)

    if (-not (Test-Path $InstallDir)) {
        New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    }

    Stop-CaptureService

    if (-not $InPlace) {
        $payload = Get-ChildItem -Path $source -File |
            Where-Object { $_.Extension -in '.dll', '.exe' }
        foreach ($file in $payload) {
            Copy-Item -Path $file.FullName -Destination $InstallDir -Force
            Write-Log ("  copied {0}" -f $file.Name)
        }
    }

    Install-Driver -Source $source
    if ($script:ExitCode -ne 0) { return }

    if (-not (Install-Service -Directory $InstallDir)) { return }
    if (-not (Register-Source -Directory $InstallDir)) { return }
    if (-not (Add-VirtualCamera -Directory $InstallDir)) { return }

    Test-Result -Directory $InstallDir | Out-Null

    Write-Log ''
    Write-Log 'Done. Open a camera app (or https://webrtc.github.io/samples/src/content/devices/input-output/)' 'OK'
    Write-Log 'and pick "PS3 Eye Windows Virtual Camera".' 'OK'
    Write-Log ("Log: {0}" -f $script:LogPath)
}

function Invoke-Uninstall {
    Write-Log 'Removing the PS3 Eye native camera...'

    $registrar = Join-Path $InstallDir 'PS3EyeVCamRegistrar.exe'
    if (Test-Path $registrar) {
        Invoke-Exe $registrar @('remove') -OkCodes @(0, 1) | Out-Null
    } else {
        Write-Log 'Registrar not found; the camera entry must be removed manually.' 'WARN'
    }

    $dll = Join-Path $InstallDir 'PS3EyeVCamSource.dll'
    if (Test-Path $dll) {
        Invoke-Exe (Join-Path $env:SystemRoot 'System32\regsvr32.exe') @('/u', '/s', "`"$dll`"") | Out-Null
    }

    $serviceExe = Join-Path $InstallDir 'PS3EyeCaptureService.exe'
    if (Test-Path $serviceExe) {
        Invoke-Exe $serviceExe @('--uninstall') -OkCodes @(0, 1) | Out-Null
    } else {
        Stop-CaptureService
        & sc.exe delete $ServiceName 2>&1 | Out-Null
    }

    $msi = $DriverMsi
    if (-not $msi) {
        $msi = Get-ChildItem -Path $PSScriptRoot -Filter '*.msi' -File -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $SkipDriver -and $msi -and (Test-Path $msi)) {
        Write-Log 'Uninstalling the PS3 Eye Universal driver + DirectShow filter...'
        Invoke-Exe 'msiexec.exe' @('/x', "`"$msi`"", '/qn', '/norestart') -OkCodes @(0, 3010, 1605) | Out-Null
    }

    if (-not $InPlace -and (Test-Path $InstallDir)) {
        Remove-Item -Path $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path $InstallDir) {
            Write-Log 'Could not delete the install folder (files still loaded); reboot and remove it.' 'WARN'
        }
    }

    Write-Log 'Uninstall finished.' 'OK'
}

# ------------------------------------------------------------------ run -----

if (-not (Test-IsAdmin)) {
    exit (Invoke-Elevated)
}

Initialize-Log

$build = [Environment]::OSVersion.Version.Build
if (-not $Uninstall -and -not $IgnoreOsCheck -and $build -lt 22000) {
    Fail ("This build is Windows build {0}. The virtual camera API (MFCreateVirtualCamera) exists only on Windows 11 build 22000 and newer." -f $build)
    Fail 'On Windows 10 use the DirectShow filter instead (PS3 Eye Universal in OBS/Brave with the DirectShow flag).'
    Fail 'Pass -IgnoreOsCheck to install anyway (the camera will not enumerate).'
    exit 1
}

try {
    if ($Uninstall) { Invoke-Uninstall } else { Invoke-Install }
} catch {
    Fail ("Unexpected error: {0}" -f $_.Exception.Message)
    Fail ("At: {0}" -f $_.InvocationInfo.PositionMessage)
}

exit $script:ExitCode
