# PS3 Eye as a native Windows camera (Windows 11)

Turns the PS3 Eye into a **real Windows camera**: Media Foundation enumerates it,
so Brave, Chrome, Edge, Firefox, Teams, the Windows Camera app and every other
app list it like any webcam. No OBS, no DirectShow flag, no bridge process in
front of the browser.

Two routes live here, same frame source (the capture service), different device
layer:

| Route | What Windows sees | Needs | Folder |
|---|---|---|---|
| **A. Virtual camera API** (default) | a camera published by the Frame Server | Windows 11 22000+ | this folder |
| **B. AVStream kernel driver** | a real camera device node | WDK + signed driver, works on Win10 too | `KernelDriver/` |

Both are fed by `PS3EyeCaptureService`, which owns the PS3 Eye over WinUSB. Pick
B if you want the lowest-level, most native result and can deal with driver
signing; pick A for a user-mode install with no driver at all.

`Installer/Install-PS3EyeNative.ps1` installs route A end to end (driver MSI,
capture service, COM source, camera) and `Installer/PS3EyeNativeSetup.iss` wraps
it into a single `Setup.exe`. Route B has its own installer:
`KernelDriver/Install-PS3EyeDriver.ps1`.

## How it works

```
PS3 Eye (USB, WinUSB)
  └─ PS3EyeCaptureService.exe     owns the device, writes RGB frames to
     (LocalSystem, session 0)     shared memory (single USB client only)
       └─ PS3EyeVCamSource.dll    Frame Server custom media source
          (Local Service,        (IMFMediaSourceEx) reads shared memory
           session 0)
            └─ Windows Camera Frame Server
               └─ any MF app: browser, Teams, Camera app
```

`PS3EyeVCamRegistrar.exe install` calls `MFCreateVirtualCamera()` with
`MFVirtualCameraType_SoftwareCameraSource`, so Windows itself publishes the
camera (`KSCATEGORY_VIDEO_CAMERA`, `KSCATEGORY_VIDEO`, `KSCATEGORY_CAPTURE`).
The friendly name is shown with a `Windows Virtual Camera` suffix, which is
added by the pipeline.

The source DLL reuses `../MediaFoundationSource/PS3EyeMediaSource.cpp` — only the
COM hosting/registration layer is new.

## Requirements

- **Windows 11 build 22000 or newer.** On Windows 10 the virtual camera API does
  not exist; the DirectShow filter path documented in the repo root is the only
  option there.
- Windows SDK **10.0.22621 or newer** (`mfvirtualcamera.h`, `mfsensorgroup.lib`).
- Visual Studio 2022 with the C++ workload.
- Administrator rights (HKLM COM registration, `AllUsers` camera, service).

## Build

1. Build `../MediaFoundationSource/PS3EyeCaptureService.vcxproj` (x64).
2. Open `PS3EyeWin11.sln`, build `Release | x64`.
3. Output lands in `bin\x64\Release\`:
   - `PS3EyeVCamSource.dll`
   - `PS3EyeVCamRegistrar.exe`

The DLL links the static CRT on purpose: the Frame Server loads it under Local
Service, where a missing VC++ redistributable shows up as a `CoCreateInstance`
failure.

## Install

Run an elevated prompt from `bin\x64\Release\`.

```cmd
:: 1. capture service (owns the PS3 Eye)
PS3EyeCaptureService.exe --install

:: 2. COM media source (HKLM, needed by the Frame Server)
regsvr32 PS3EyeVCamSource.dll

:: 3. publish the camera
PS3EyeVCamRegistrar.exe install
```

`PS3EyeVCamRegistrar.exe status` lists the MF capture devices and whether the
capture service is running. If the camera is listed but frames are black, the
service is stopped (`sc start PS3EyeCaptureService`).

Test: open `https://webrtc.github.io/samples/src/content/devices/input-output/`
in Brave and pick **PS3 Eye Windows Virtual Camera**. While OBS or the
DirectShow filter holds the device, the capture service cannot open it — the
PS3 Eye is a single-client USB device.

Only the *USB* side is single-client. Consumers of the capture service are not:
several apps can read the same shared frame at once, and the kernel driver
(Route B) allows up to 8 simultaneous camera instances for exactly that reason.

## Uninstall

```cmd
PS3EyeVCamRegistrar.exe remove
regsvr32 /u PS3EyeVCamSource.dll
PS3EyeCaptureService.exe --uninstall
```

## Notes and limits

- Frame format is **640x480 RGB24 @ 30 fps** (what the driver delivers). Many
  apps prefer NV12; if a specific app rejects the camera, exposing NV12
  (custom `IMFStreamDescriptor` + conversion in `CaptureThreadProc`) is the next
  step — deliberately not guessed at here.
- Camera controls (gain, exposure, white balance, LED) are not forwarded to
  apps; only video frames are.
- Shared memory is created by the capture service in session 0, which is where
  the Frame Server's Local Service process can open it. Keep the capture service
  running as a service; a per-user copy will not be visible to the Frame Server.
- Debugging: the media source loads inside `FrameServerMonitor` (camera
  creation/enumeration) and `FrameServer` (streaming). Attach a debugger to
  those services; `OutputDebugString` messages go to DebugView.
