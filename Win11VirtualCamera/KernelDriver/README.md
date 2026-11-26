# PS3 Eye as a real camera device (AVStream kernel driver)

The lowest-level route in this repo: a root-enumerated AVStream minidriver
(`PS3EyeVCamKernel.sys`) that registers a genuine camera device under
`KSCATEGORY_VIDEO_CAMERA`. Every app — DirectShow, Media Foundation, Brave,
Chrome, Firefox, Teams, the Windows Camera app — sees an ordinary webcam.
No Windows 11 virtual-camera API, no COM registration, no flag toggles, and it
works on Windows 10 as well as Windows 11.

```
PS3 Eye (WinUSB)
  └─ PS3EyeCaptureService.exe      owns the USB device, writes RGB24 frames
     (LocalSystem, session 0)      into the section "PS3EyeSharedFrame"
       └─ PS3EyeVCamKernel.sys     maps that section, feeds KS pin buffers
          (AVStream camera)        640x480 RGB24 @ 30 fps
            └─ KSCATEGORY_VIDEO_CAMERA device node
               └─ any app
```

The driver does not talk USB; the capture service stays as the single owner of
the camera (the PS3 Eye accepts one client only). The driver only reads the
frame section, so it needs no WDM/USB stack, no DMA and no interrupts.

## Several apps at once

This is where the shared-frame design pays off. The USB device still has exactly
one owner, but the pin allows up to `VCAM_MAX_PINS` (8) concurrent instances, so
Brave, Teams and ffmpeg can hold the camera simultaneously; each gets its own
copy of the same captured frame, and each tracks its own frame number so no app
starves another. One 30 fps timer drives all of them.

Reciprocally, the capture service only captures while somebody is watching. It
used to learn that from the user-mode client count in the frame header, which a
kernel reader cannot bump (it maps the section read-only) - so it would have sat
idle and delivered black frames forever. The driver now signals the manual-reset
event `PS3EyeKernelReader` while any pin is open, and the service counts that as
a reader alongside `clientCount`.

## Build

Needs Visual Studio 2022 + the **WDK** (Windows Driver Kit), x64 only.

1. Open `../PS3EyeWin11.sln`, build the `PS3EyeVCamKernel` project
   (`Release | x64`). Output: `bin\x64\Release\PS3EyeVCamKernel.sys`.
2. Copy `PS3EyeVCamKernel.sys` next to `PS3EyeVCamKernel.inf` and the generated
   `PS3EyeVCamKernel.cat` (Inf2Cat + signing are off in the project, so produce
   the catalog yourself: `Inf2Cat /driver:<dir> /os:10_X64`, then sign).

The driver project deliberately does not sign or catalog automatically — signing
is a machine/company decision.

## Signing

Windows will not load an unsigned kernel driver.

**Development (test signing)**

```cmd
:: elevated, then reboot yourself - this repo's scripts never reboot
bcdedit /set testsigning on
```

After the reboot, sign the build with a test certificate and install:

```cmd
makecert -r -pe -ss PrivateCertStore -n "CN=PS3EyeTest" PS3EyeTest.cer   :: or use New-SelfSignedCertificate
signtool sign /v /s PrivateCertStore /n PS3EyeTest /fd SHA256 PS3EyeVCamKernel.sys
Inf2Cat /driver:. /os:10_X64
signtool sign /v /s PrivateCertStore /n PS3EyeTest /fd SHA256 PS3EyeVCamKernel.cat
```

**Distribution:** attestation-sign the `.sys` and `.cat` through the Partner
Center (no WHQL test suite needed for a driver like this).

## Install

```powershell
# elevated; does not reboot
powershell -ExecutionPolicy Bypass -File .\Install-PS3EyeDriver.ps1
```

It stages the package with `pnputil`, creates the `ROOT\PS3EyeVCam` device node
via SetupAPI (no devcon needed), binds the driver, and verifies the node. If test
signing is off it prints the `bcdedit` line and stops — it never reboots.

Uninstall:

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-PS3EyeDriver.ps1 -Uninstall
```

Then start the frame source:

```cmd
"C:\Program Files\PS3 Eye\PS3EyeCaptureService.exe" --install
```

If you previously installed the user-mode virtual camera
(`PS3EyeVCamRegistrar.exe install`), remove it first, otherwise Windows lists two
cameras:

```cmd
"C:\Program Files\PS3 Eye\PS3EyeVCamRegistrar.exe" remove
regsvr32 /u "C:\Program Files\PS3 Eye\PS3EyeVCamSource.dll"
```

## Verify

```powershell
pnputil /enum-devices /instanceid "ROOT\PS3EyeVCam"
Get-PnpDevice -Class Media | Where-Object { $_.FriendlyName -like '*PS3*' }
```

In an app, pick **PS3 Eye Camera**. Black frames mean the capture service is not
running or the camera is held by OBS. Kernel-side logging goes to
`DbgPrintEx`/DebugView (`PS3EyeVCam:`). When there is no debugger attached, the
`Diag*` values under the driver's service key say how far a client got; the list
of values, and the meaning of each `DiagDeliverStage` code, is in
`COMPATIBILITY.md`.

## Test it — the four stages

Do this on a VM or a spare machine. A broken kernel driver bugchecks the host
(three of them did during bring-up), and the camera's isochronous stream needs a
real host controller to reach full frame rate — see `COMPATIBILITY.md`.

**Stage 0 - build machine.** Visual Studio 2022 + WDK, x64. Build
`PS3EyeVCamKernel` from `../PS3EyeWin11.sln`.

**Stage 1 - VM that survives mistakes.** Hyper-V or VMware, Windows 10 or 11,
checkpoint taken first, kernel debugging enabled:

```cmd
bcdedit /debug on
bcdedit /dbgsettings net hostip:<your-ip> port:50000 key:1.2.3.4
bcdedit /set testsigning on
```

Attach WinDbg with the matching key (or use the serial/pipe transport) so a
bugcheck gives you `!analyze -v` instead of a reboot loop. Debug messages go to
DebugView or the debugger: `PS3EyeVCam: ...`.

**Stage 2 - does it load at all?** Install (see above), then:

```powershell
sc query PS3EyeVCamKernel                 # must be RUNNING, not STOPPED
pnputil /enum-devices /instanceid "ROOT\PS3EyeVCam"
Get-PnpDevice -Class Media | Where-Object { $_.FriendlyName -like '*PS3*' }
```

FriendlyName comes from the INF, not the driver, so a missing name is an INF
bug. Device Manager must show the camera with no yellow triangle.

If the device starts with a yellow triangle (Code 10/37/39), the first things to
try: add `HKR,,DevLoader,,*ntkern` and `HKR,,NTMPDriver,,PS3EyeVCamKernel.sys`
to an `AddReg` section (some AVStream INFs carry these legacy hints), or check
the INF's `AddService`/`ServiceBinary` path in
Event Viewer -> Windows Logs -> System (Service Control Manager).

**Stage 3 - does it enumerate a pin?** Before involving browsers, check the
format with a plain DirectShow tool (GraphStudioNext, or ffmpeg with dshow
support):

```cmd
ffmpeg -f dshow -list_options true -i video="PS3 Eye Camera"
```

If the camera is listed but no format is offered, the data range is wrong —
most often `KS_DATARANGE_VIDEO.VideoInfoHeader` or `DataRange.FormatSize`.
If it is not listed at all, suspect `Categories` and `ReferenceGuid`.

**Stage 4 - frames.** Start the capture service, then grab one frame:

```cmd
ffmpeg -f dshow -i video="PS3 Eye Camera" -frames:v 1 frame.png
```

Black frames mean the service is not capturing (not installed, camera held by
OBS, or the `PS3EyeKernelReader` event not signalled), `DataOffset`/`DataSize`
wrong, or the pin never reaches `KSSTATE_RUN`. Green/garbled frames mean stride
or RGB/BGR order.

For the multi-app case, open the same camera in two clients at once (e.g. Brave
and `ffmpeg`) and check that neither stalls and both keep updating.

## Status — read this before trusting the driver

- **Builds, loads, enumerates and streams.** Verified in a Windows 10 19044 VM
  with test signing; see `COMPATIBILITY.md` for the tested configuration, the
  measurements and the limits. Chrome lists *PS3 Eye Camera* and renders 640x480
  frames through Media Foundation.
- Fixed during bring-up (each was a build break or an observed bugcheck): wrong
  `KsCreateFilterFactory` call (it is `NTSTATUS`, takes a `PDEVICE_OBJECT` and 7
  arguments) — the factory is now created by AVStream from
  `KSDEVICE_DESCRIPTOR` instead; `KSPIN_DISPATCH.SetStatus` does not exist (it is
  `SetDeviceState`); `KSPIN_DESCRIPTOR_EX.PinDescriptor` is a struct by value,
  not a pointer; `KSFILTER_DESCRIPTOR` has no `DescriptorId` or
  `ConnectionDescriptors` but **requires** `PinDescriptorSize` (omitting it
  makes AVStream walk the descriptor array off the end); the missing
  `KS_DATARANGE_VIDEO.VideoInfoHeader` (no format would ever be offered);
  `KSMEDIUM` is not a type (`KSPIN_MEDIUM`) and the medium-set GUID was wrong;
  `KeInitializeTimer` cannot be used with `KeSetTimerEx` (needs
  `KeInitializeTimerEx`); `VcamPinClose` could unmap the shared section while a
  DPC was still reading it, now `KeFlushQueuedDpcs()` before unmap.
- Fixed while streaming (all three bugchecked the machine, see
  `COMPATIBILITY.md`): the frame view is mapped with `MmMapViewInSystemSpace`
  because a process-local `ZwMapViewOfSection` is only valid in the context that
  mapped it, and the delivery DPC runs anywhere; the per-call registry logging
  that faulted on an unterminated value name is gone; and a restarted capture
  service is picked up again instead of leaving the driver on a stale section
  and reader event.
- Still open by design:
  - Single format: 640x480 RGB24 @ 30 fps. NV12 needs a conversion pass in
    `VcamDeliverFrame` and is not implemented.
  - Camera controls (gain, exposure, white balance, LED) are not exposed as KS
    properties; only frames are.
  - Tearing is possible: the driver reads the shared frame without the
    user-mode mutex (it cannot take one in a DPC) and relies on the frame
    counter to skip partial updates.
  - Multi-instance is handled with a slot table, a run-state refcount and a
    `KeFlushQueuedDpcs()` in the close path. That ordering is the part most
    worth reviewing on the first real build: clearing the slot and flushing is
    what stops a DPC from touching a pin AVStream is about to free.
- Microsoft's WDK sample `avscamera`/`avshws` implements the same architecture
  and is the reference to diff against when something does not enumerate.
