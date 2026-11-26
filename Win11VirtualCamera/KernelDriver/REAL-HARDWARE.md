# Running the native camera driver on real hardware

The VM route is exhausted: an emulated host controller in this setup never
completes the camera's isochronous transfers, so it can prove enumeration and the
delivery path but not the stream (see `COMPATIBILITY.md`). Real hardware is where
the remaining question gets answered — full 30 fps, and the driver under real
timing.

This costs **two reboots**: one to turn test signing on, one to turn it off
again. Nothing here reboots the machine for you.

`Install-PS3EyeDriver.ps1` never reboots either: if test signing is off it prints
the `bcdedit` line and stops, so the reboot stays your decision.

## 0. Have ready

- A Windows 10 19044+ / Windows 11 x64 machine, admin rights.
- The driver package: `PS3EyeVCamKernel.sys`, `.inf`, `.cat`, plus
  `Install-PS3EyeDriver.ps1` and `PS3EyeCaptureService.exe`
  (CI artifact: `PS3EyeVCam.zip` from the `driver-build` workflow; or build it
  yourself — see `README.md`).
- The PS3 Eye, plugged in directly — not through a hub.
- ~15 minutes and the two reboots.

## 1. Safety net first

Do all of this before installing anything.

```powershell
# restore point
Checkpoint-Computer -Description "before PS3Eye driver" -RestorePointType MODIFY_SETTINGS

# small memory dumps, and reboot after a bugcheck so the machine comes back
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' -Name CrashDumpEnabled -Value 3
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' -Name AutoReboot -Value 1
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' CrashDumpEnabled,AutoReboot
```

Why this is enough: the crash-capable paths (`VcamConnect` on pin open, the
delivery DPC on a running pin) only run while an application has the camera open.
Nothing opens the camera at boot, and the driver's load path is just
`KsInitializeDriver`, so a bugcheck cannot put the machine in a boot loop. The
realistic worst case is: open a camera app, it bugchecks, the machine reboots to
a working desktop.

The driver service is `StartType=3` (demand) with `ErrorControl=1` (normal), so a
load failure is logged rather than fatal. Keep it off the boot path anyway:

```powershell
sc.exe config PS3EyeCaptureService start= demand
```

## 2. Test signing on, then reboot

```cmd
bcdedit /set testsigning on
shutdown /r /t 0
```

After the reboot:

```cmd
bcdedit /enum {current} | findstr /i testsigning
```

Must say `testsigning Yes`. The desktop also shows "Test Mode" in the corner.

## 3. Trust the test certificate

Windows still has to accept the signature. For a test-signed build:

```cmd
certutil -addstore Root PS3EyeTest.cer
certutil -addstore TrustedPublisher PS3EyeTest.cer
```

## 4. Install

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-PS3EyeDriver.ps1
sc.exe config PS3EyeCaptureService start= demand
```

The installer stages the package with `pnputil`, creates the `ROOT\PS3EyeVCam`
device through the Software Device API, binds the driver and verifies the node.
If it stops complaining about the signature, test signing is not actually on —
check step 2 before forcing anything.

If test signing has to stay off for now, keep the device node disabled between
sessions so the driver is not loaded at all:

```cmd
pnputil /disable-device /instanceid "ROOT\PS3EyeVCam"
pnputil /enable-device  /instanceid "ROOT\PS3EyeVCam"   :: when you want to test
```

## 5. Test

```powershell
sc.exe start PS3EyeCaptureService
Get-Content C:\ProgramData\PS3Eye\capture.log -Tail 20 -Wait
```

Open the camera in any app (Windows Camera, or the repo's `cam.html` in Brave /
Chrome). What the log should say, in order:

```
start: shared memory created
reader attached, 1 device(s) found
camera started (init ok)
first frame captured
capture: 60 frames in 2s, frameNumber=..., clients=0, kernelReader=1
```

`60 frames in 2s` is the 30 fps you want, and it repeating every 2 seconds is the
proof the camera is delivering. On real hardware the log must **not** contain
`synthetic capture: no camera opened` — if it does, `PS3EYE_SYNTHETIC` is still
set in the machine environment and you are looking at the test pattern.

Driver side, readable without a debugger:

```powershell
$k = 'HKLM:\SYSTEM\CurrentControlSet\Services\PS3EyeVCamKernel'
Get-ItemProperty $k | Select-Object DiagDeliverStage,DiagProcess,DiagRun,DiagFramesDelivered
```

`DiagDeliverStage` should be the delivered code, `DiagProcess` should climb by
about 30 per second (a second counter that runs with the pin), and
`DiagFramesDelivered` should rise as the app paints. The full list of `Diag*`
values and stage codes is in `COMPATIBILITY.md`.

Expected behaviour worth checking explicitly, because the VM could not:

- **Picture and rate.** 640x480, ~30 fps, correct colours, no green/garbled
  frame (that would be stride or RGB/BGR order).
- **Reconnect.** Close the app, restart the capture service, reopen the app:
  frames come back without touching the driver. This is verified in the VM, but
  with real frames it is the first time the disconnect runs while a delivery DPC
  is live.
- **Two apps at once.** Open the camera in two clients; both keep updating and
  neither starves the other.

## 6. If it bugchecks

1. Let it reboot. Do not reopen the camera until you have read the dump.
2. Collect `C:\Windows\Minidump\*.dmp` (and note the stop code on the blue
   screen). `!analyze -v` names the faulting frame directly.
3. Roll back:

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-PS3EyeDriver.ps1 -Uninstall
```

## 7. Return the machine to normal

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-PS3EyeDriver.ps1 -Uninstall
& "C:\Program Files\PS3 Eye\PS3EyeCaptureService.exe" --uninstall
certutil -delstore Root PS3EyeTest
certutil -delstore TrustedPublisher PS3EyeTest
sc.exe config PS3EyeCaptureService start= auto   # only if you keep the service
```

```cmd
bcdedit /set testsigning off
shutdown /r /t 0
```

"Test Mode" must be gone from the desktop afterwards and
`bcdedit /enum {current}` must not report `testsigning Yes`.

## What to report back

- The `capture:` lines (frame rate is the headline number).
- `DiagDeliverStage` / `DiagFramesDelivered` while an app is open.
- Whether the reconnect case (step 5) resumed on its own.
- If it faulted: the stop code and the `.dmp`.

A frame rate of ~1 per 2 seconds instead of 30 means the USB side is starved, not
the driver — the same build produces 30 fps from the synthetic source, and the
emulated-controller measurements in `COMPATIBILITY.md` show what that starvation
looks like.
