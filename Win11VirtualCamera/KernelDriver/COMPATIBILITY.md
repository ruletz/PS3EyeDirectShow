# Compatibility and status

What the AVStream camera driver (`PS3EyeVCamKernel.sys`) has actually been run
against, what it needs to work, and where it stops. Everything below was
measured; nothing here is inferred from the source.

## Tested configuration

| Component | Value |
|---|---|
| Host | Windows 11, VirtualBox 7.1.16 and 7.2.22 (isochronous streaming needs `hypervisorlaunchtype off`) |
| Guest | Windows 10 Enterprise LTSC 19044 (x64), test signing on |
| Driver | `PS3EyeVCamKernel.sys` 1.0.0.147, test-signed with the repo certificate |
| Frame source | `PS3EyeCaptureService.exe` (LocalSystem, session 0), PS3 Eye over WinUSB |
| Camera | PS3 Eye, USB `1415:2000`, "USB Camera-B4.04.27.1" |
| Clients | Chrome (`file:///…/cam.html` test page, `getUserMedia`), Media Foundation via the KS proxy |

## What works

- **Enumeration.** Windows publishes `ROOT\PS3EYEVCAM\0000` with FriendlyName
  *PS3 Eye Camera* under `KSCATEGORY_VIDEO_CAMERA`. Chrome lists it as
  `PS3 Eye Camera` and `getUserMedia({video: {width: 640, height: 480}})`
  succeeds with `track.getSettings()` reporting 640x480 at 30 fps.
- **Pixel data.** The rendered image is a correct, correctly-coloured frame —
  no stride, BGR/RGB or byte-order problems, and not a black frame
  (`pixels avg ≈ 95` on the test page).
- **Format negotiation.** Media Foundation asks for `VIDEOINFOHEADER2` and
  accepts the single `RGB24` range the driver offers; DirectShow-style
  `VIDEOINFOHEADER` requests get the same range back.
- **Streaming state machine.** Pin create → `SetDataFormat` → `KSSTATE_RUN`
  reaches `VcamDeliverFrame` with a valid buffer: the buffer size matches
  `FrameExtent`, and frames are delivered when the frame counter moves.
- **On-demand capture.** The service only captures while a reader exists. A
  kernel client therefore needs the `PS3EyeKernelReader` event (the driver sets
  it on pin create and on every RUN), and it works: the service logs
  `kernelReader=1` while the app is open.
- **Service restart.** The driver used to keep the section view and the reader
  event from the previous service instance, which made a restarted service
  deliver a frame number that never advanced. It now drops both on the first
  pin and reopens the event on every RUN. Verified with frames flowing: with a
  client open, the service was killed and restarted, the client was closed and
  reopened, and the new session reconnected on its own — `DiagRun` went 2 → 3,
  `DiagDeliverStage` returned to `DELIVERED`, and the page resumed painting at
  30 fps without the driver being reloaded or the device touched.

## Verifying without a working USB stream

The path downstream of USB can be checked on its own, which is worth having
when the machine cannot pass isochronous transfers through (see below). Set the
machine environment variable `PS3EYE_SYNTHETIC=1` and restart the capture
service: it publishes a moving test pattern at 640x480x30 instead of opening the
camera, and logs `synthetic capture: no camera opened` so the log never leaves
doubt about which source is in use. Everything after that point is real — the
shared section, the kernel driver, Media Foundation, the browser.

Measured that way in the VM: the service wrote `61 frames in 2s` (~30 fps), the
driver reported a delivered frame, and Chrome's test page painted
`OK 640x480 fps=30 painted=100 secs=4.0`, `pixels avg=166.3` with a visibly
moving pattern. That isolates a frame-rate problem to the USB side, because the
same build delivers 30 fps when the source is not the camera.

## When the stream dies

`PS3EYECam::getFrame()` waits for a frame with no timeout, so a camera that stops
mid-stream (cable pulled, port wedged) parks the capture thread inside it for
ever: the service sits in `STOP_PENDING` and only `taskkill` clears it, with the
camera's LED still lit. The service now watches for that and gives up on the
process instead:

- A watchdog thread checks the last frame time once a second. After **10 s** of
  silence while a pin is open it logs
  `capture stalled 11 s after 30 frames (kernelReader=1): restarting the service process`
  and calls `ExitProcess(1)` — the only way out of a blocked capture thread,
  since `stop()` waits on that same thread.
- `--install` registers SCM recovery actions: restart after 5 s, three attempts
  per 24 h. A camera that is genuinely gone therefore stops churning instead of
  restarting forever, and a camera that was only interrupted comes back on its
  own once it is delivering again.
- Verified in the VM with the synthetic source in `stall` mode (30 frames, then
  silence): the log line above, service `Stopped`, then `Running` with a new PID
  and a fresh `start: shared memory created` 5 s later.

## Requirements

- Windows 10 19044+ or Windows 11, x64.
- Test signing (development) or an attestation-signed `.sys`/`.cat`.
- `PS3EyeCaptureService.exe` must be running: it owns the USB device and writes
  the frames. Without it the camera enumerates and streams black.
- The PS3 Eye must be bound to WinUSB. While the capture service runs, no other
  program can own the USB device — including OBS and the DirectShow filter's
  own USB path. Several *consumers* of the shared section are fine.

## Limits

- **One format:** 640x480 RGB24 at 30 fps. No NV12 (needs a conversion pass in
  `VcamDeliverFrame`), no other resolutions.
- **No camera controls.** Gain, exposure, white balance and the LED are not
  exposed as KS properties; the service sets autogain/AWB itself.
- **Tearing is possible.** The driver reads the shared frame from a DPC without
  the user-mode mutex (it cannot take one at DISPATCH_LEVEL) and relies on the
  frame counter to skip partially written frames.
- **Two apps can see different frames** of the same 30 fps stream, but each pin
  tracks its own frame number, so no client starves another. Up to 8 pins.

## Isochronous bandwidth is the real bottleneck

The PS3 Eye is an isochronous USB 2.0 device: 640x480 raw Bayer at 30 fps is
about 18 MB/s of continuous isochronous traffic. Emulated host controllers do
not sustain that.

Measured in the VM above, with the driver ticking normally:

| Measurement | Value |
|---|---|
| `DiagProcess` (delivery attempts) | ~30/s — the timer and pin callbacks are healthy |
| `DiagDeliverStage` | `4` (`VCAM_STAGE_SAME_FRAME`) — no new frame exists to deliver |
| `capture: N frames in 2s` (service log) | **1 frame per 2 s**, then 0 |
| Chrome `requestVideoFrameCallback` count | 0 new frames in 4 s (still picture) |

So when the picture freezes, the fault is upstream of the driver. Two failure
modes were observed:

1. **Throttled stream.** The service receives ~0.5 fps instead of 30 fps.
2. **Stalled stream that never recovers.** After a few hundred frames the
   isochronous pipes stop completing. `PS3EYECam::getFrame()` then blocks
   forever, which leaves the service in `STOP_PENDING` (it cannot even be
   stopped, only killed) and produces no frames at all. Detaching and
   re-attaching the USB device in VirtualBox brought back one frame every two
   seconds; only a physical re-plug or a host-side controller reset clears it.

Neither is reachable from the driver: it never touches USB. Expect full-rate
capture on real hardware with the camera on a real xHCI controller; treat any
number measured inside a VM as a lower bound.

Two more things worth knowing when the guest is virtual:

- **The emulated controller will deliver nothing at all, and neither the
  controller choice nor a device reset fixes it.** In this VM the camera
  enumerates, binds to WinUSB, and `PS3EYECam::init()` succeeds (control
  transfers are fine, the LED lights up because the device has been told to
  stream), yet not one isochronous IN ever completes — `DiagDeliverStage` stays
  at `NO_FRAME` and Chromium reports `AbortError: Timeout starting video
  source`. Switching between the USB 3.0 (xHCI) and USB 2.0 (EHCI) controllers,
  `pnputil /restart-device`, and even a physical unplug/replug were each tried
  against a healthy, freshly powered device and all produced zero frames. The
  only success ever seen here was a single 519-frame burst (~17 s at 30 fps)
  immediately after one session started, after which the stream never recovered.
  Treat the VM as a place to prove enumeration, the driver's device interface
  and the connect/deliver plumbing — not to measure frame rate.
- **The outer Windows hypervisor breaks isochronous passthrough, and turning it
  off fixes it.** With VirtualBox running in NEM mode (the host had
  VirtualMachinePlatform/WSL enabled and VBS active, so VirtualBox was a guest of
  the Windows hypervisor) the camera produced **zero** frames on either USB
  controller, through any number of device resets and VBUS cycles, on both
  VirtualBox 7.1.16 and 7.2.22. After
  `bcdedit /set {current} hypervisorlaunchtype off` and a reboot — VirtualBox then
  logging `HM: Using VT-x implementation 3.0` instead of NEM — the same VM, same
  guest, same driver streamed real frames immediately: **35–43 frames per 2 s
  (~18–21 fps)**, `DiagDeliverStage` reporting delivered, and a correct 640x480
  image in Chrome. So the emulated-controller ceiling is real but far higher than
  the old measurements suggested; those were measuring the outer hypervisor, not
  VirtualBox or this driver.
- **A stalled stream does not recover in the guest.** If the stream dies while
  the capture service is running, `PS3EYECam::stop()` waits on the stuck
  transfer thread and the service sits in `STOP_PENDING` until the process is
  killed. Kill the service before attempting any recovery; the device then needs
  a physical unplug/replug (a real VBUS cycle) before it will stream again.

## Diagnosing a stream that does not start

The driver writes counters to its own service key at PASSIVE_LEVEL
(`HKLM\SYSTEM\CurrentControlSet\Services\PS3EyeVCamKernel`), readable without
elevation:

| Value | Meaning |
|---|---|
| `DiagLoaded` | `DriverEntry` ran and the device object was created |
| `DiagOpen` / `DiagIntersect` | device open and format-intersection calls from the client |
| `DiagSetFormat` / `DiagSetFormatFail` | accepted / refused `SetDataFormat` |
| `DiagSubFormat`, `DiagFormatSize` | what the client asked for (`0xE436EB7D` = RGB24) |
| `DiagPinCreate`, `DiagRun` | pin created, pin reached `KSSTATE_RUN` |
| `DiagProcess` | delivery attempts; a static value means the timer stopped |
| `DiagConnectStatus` | `ZwOpenSection` result for `PS3EyeSharedFrame` (`0` = OK) |
| `DiagReaderEventStatus` | `ZwOpenEvent` result for `PS3EyeKernelReader` (`0` = OK) |
| `DiagDeliverStage` | last branch in `VcamDeliverFrame`: 1 no buffer pointer, 2 bad buffer, 3 frame number 0, 4 same frame, 5 bad magic, 7 outside the view, 8 buffer too small, 9 delivered |
| `DiagFrameExtent` | the client's `FrameExtent` when stage 8 was hit |

**Before blaming the driver, check that nothing else already holds the camera.**
An earlier copy of the capture service installed on the *other* machine (or the
host, if you are testing in a VM) keeps the device open: the LED stays lit, the
VM cannot attach the device properly, and the guest sees no frames. Both sides
run the same service name, so `sc query PS3EyeCaptureService` on the side you
are *not* testing is a one-line answer.

The capture service logs to `%ProgramData%\PS3Eye\capture.log` (a session-0
service has no console): shared memory creation, reader attach, device count,
camera init, first frame, capture rate every two seconds with the frame
counter, client count and kernel-reader state, and camera stop.

## Bugchecks found and fixed during bring-up

Each of these was reproduced in the VM and fixed; the driver has since run for
hours of Windows uptime without a bugcheck.

| Bugcheck | Where | Cause | Fix |
|---|---|---|---|
| `0x7E` | `VcamDeliverFrame+0x1939`, reading `header->FrameNumber` | `ZwMapViewOfSection` maps into the *current process*; the delivery DPC runs in whatever context it lands in | map once with `MmMapViewInSystemSpace` |
| `0x3B` | `VcamConnect`, reading `header->Magic` | same process-local view, reached from a different process (`ObReferenceObjectByHandle` with a wrong type) | pass `NULL` for the object type after `ZwOpenSection`, unmap with `MmUnmapViewInSystemSpace` |
| `0x3B` | device create | per-call registry logging built a value name in a fixed stack buffer that was never NUL-terminated | literal names only, counters instead of per-call logging |
| install failure | `SetupDiSetDeviceRegistryProperty` | property written as ANSI, so the instance path went in as a byte string | `CharSet.Unicode` |

## Related route

The user-mode virtual camera (`Win11VirtualCamera/`, `PS3EyeVCamSource.dll` +
`MFCreateVirtualCamera`) and this driver share the same frame source and the
same shared-memory protocol, so they cannot both own the USB device — but they
are unrelated to each other beyond that: route B does not need Windows 11, COM
registration or the Frame Server, and route A does not need a driver.
