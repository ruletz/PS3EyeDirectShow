// PS3EyeVCamKernel.c
// Minimal AVStream (KS) camera minidriver for the PS3 Eye.
//
// State: compiles with the WDK, loads on Windows 10 21H2 with `bcdedit /set
// testsigning on` plus the build's test certificate trusted, registers its
// device node and pin descriptor, and a browser (Chrome/Chromium through Media
// Foundation) opens the camera, negotiates RGB24 and runs the pin. Frames reach
// the pin only while PS3EyeCaptureService is running - see "Status" in
// README.md.
//
// This is the low-level route: instead of the Windows 11 virtual camera API it
// creates a real camera device node (root-enumerated, KSCATEGORY_VIDEO_CAMERA),
// so every app - DirectShow, Media Foundation, browsers, Teams, the Camera app
// - sees an ordinary webcam, on Windows 10 as well as 11.
//
// Frames come from PS3EyeCaptureService (which owns the USB device through
// WinUSB and writes RGB24 frames into the named section "PS3EyeSharedFrame").
// The driver only reads that section, so it needs no USB stack of its own.
//
// Format: 640x480 RGB24 @ 30 fps, one pin, up to VCAM_MAX_PINS concurrent
// instances - every app opening the camera reads the same captured frame, so
// several apps can stream at once off the single USB device.
//
// The filter factory is created by AVStream from g_DeviceDescriptor (the normal
// path for a PnP AVStream minidriver), so this file never calls
// KsCreateFilterFactory. See README.md for building and signing; the driver
// must be test-signed (or attestation/WHQL-signed) before Windows loads it.

// ks.h's C fallback for DEFINE_GUIDSTRUCT is "EXTERN_C const CDECL GUID name".
// MSVC's C front end rejects a calling convention on a variable, and the CDECL
// it wants is only declared in user-mode winnt.h, so ks.h does not compile as C
// as shipped. Declaring the GUIDs without a calling convention is enough here:
// this driver defines the few GUID values it uses itself (kCategoryVideoCamera,
// kCategoryCapture, kPinCategoryCapture), so no ks.h GUID symbol is ever needed
// at link time.
// (In C++ builds none of this applies: there the GUIDs are uuid-decorated
// structs that resolve at compile time.)
#if !defined(DEFINE_GUIDEX)
#define DEFINE_GUIDEX(name) EXTERN_C const GUID name
#endif

#if !defined(CDECL)
#define CDECL __cdecl
#endif

#include <ntddk.h>

// ks.h and ksmedia.h pick between user-mode and kernel declarations on
// _NTDDK_; without it they fall into the user-mode branches and ask for DWORD,
// BYTE, MAX_PATH, BOOL, FLOAT and the wtypes.h block, none of which a kernel
// build has. The WDK does not define it for us, so set it before the KS
// headers, which is what those "#if defined(_NTDDK_)" sections expect.
#if !defined(_NTDDK_)
#define _NTDDK_
#endif

// Both KS headers still describe user-mode-flavoured structures - the
// DirectSound3D ones use BOOL and FLOAT, the audio ones DWORD, WORD and BYTE, a
// topology endpoint id is MAX_PATH long, and KS_VIDEOINFOHEADER is built on
// RECT - but those names only exist in user-mode windef.h, which a driver never
// includes. Supply the same underlying types so the headers parse; this driver
// uses none of them.
#if !defined(_WINDEF_)
typedef LONG BOOL;
typedef float FLOAT;
typedef ULONG DWORD;
typedef USHORT WORD;
typedef UCHAR BYTE;
typedef ULONGLONG DWORDLONG;
typedef unsigned int UINT;
typedef struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT, *PRECT;
typedef struct tagSIZE {
    LONG cx;
    LONG cy;
} SIZE, *PSIZE;
#endif

// ks.h declares KsAcquireCachedMdl/KsReleaseCachedMdl with WINAPI and ksmedia.h
// types PRTAUDIOGETPOSITION with CALLBACK; the kernel headers use NTAPI and
// define neither.
#if !defined(WINAPI)
#define WINAPI __stdcall
#endif

#if !defined(CALLBACK)
#define CALLBACK __stdcall
#endif

#if !defined(MAX_PATH)
#define MAX_PATH 260
#endif

// The kernel-mode half of ks.h annotates its prototypes with SAL
// (_IRQL_requires_max_, _Outptr_result_maybenull_, ...), which ntddk.h does not
// pull in.
#include <sal.h>

#include <ks.h>
#include <ksmedia.h>

#define PS3EYE_WIDTH 640
#define PS3EYE_HEIGHT 480
#define PS3EYE_BYTES_PER_PIXEL 3
#define PS3EYE_STRIDE (PS3EYE_WIDTH * PS3EYE_BYTES_PER_PIXEL)
#define PS3EYE_FRAME_SIZE (PS3EYE_STRIDE * PS3EYE_HEIGHT)
#define PS3EYE_FPS 30
#define PS3EYE_FRAME_INTERVAL_MS (1000 / PS3EYE_FPS)
#define PS3EYE_FRAME_INTERVAL_100NS (10000000LL / PS3EYE_FPS)
#define PS3EYE_BITS_PER_SECOND (PS3EYE_FRAME_SIZE * 8 * PS3EYE_FPS)
#define PS3EYE_MAGIC 0x45335350u

// Section created by PS3EyeCaptureService (session 0, LocalService/SYSTEM).
#define PS3EYE_SECTION_NAME L"\\BaseNamedObjects\\PS3EyeSharedFrame"
// The service only captures while somebody is watching. User-mode readers
// register by bumping the client count in the header; a kernel reader cannot
// (the section is mapped read-only), so it signals this event instead.
#define PS3EYE_READER_EVENT_NAME L"\\BaseNamedObjects\\PS3EyeKernelReader"

// How many apps may hold the camera open at the same time.
#define VCAM_MAX_PINS 8

//------------------------------------------------------------------------------
// Shared memory layout - must match PS3EyeSharedMemory.h byte for byte.
//------------------------------------------------------------------------------

#pragma pack(push, 1)
typedef struct _PS3EYE_FRAME_HEADER {
    ULONG Magic;
    ULONG Version;
    ULONG Width;
    ULONG Height;
    ULONG Stride;
    ULONG Format;
    ULONGLONG FrameNumber;
    ULONGLONG Timestamp;
    ULONG DataOffset;
    ULONG DataSize;
    ULONG ServerPid;
    volatile LONG ClientCount;
    ULONG Reserved[4];
} PS3EYE_FRAME_HEADER;
#pragma pack(pop)

//------------------------------------------------------------------------------
// GUIDs, written out locally so the driver does not depend on INITGUID or on
// which header version supplies DEFINE_GUIDNAMED.
//------------------------------------------------------------------------------

static const GUID kMajorTypeVideo = {
    0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kSubtypeRgb24 = {
    0xe436eb7d, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
// KSCATEGORY_VIDEO_CAMERA / KSCATEGORY_CAPTURE (values from ks.h).
static const GUID kCategoryVideoCamera = {
    0xe5323777, 0xf976, 0x4f5b, {0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44}};
static const GUID kCategoryCapture = {
    0x65e8773d, 0x8f56, 0x11d0, {0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};

// PINNAME_VIDEO_CAPTURE. A pin's Category is a *pin* category, not a filter
// one: Media Foundation and DirectShow locate the capture pin of a device by
// asking for this GUID. Publishing the filter's KSCATEGORY_CAPTURE here instead
// leaves the device with a pin nobody can find, which is what a browser reports
// as "could not start video source" without ever creating a pin.
static const GUID kPinCategoryCapture = {
    0xfb6c4281, 0x0353, 0x11d1, {0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba}};

// Filter identity (also referenced by PS3EyeVCamKernel.inf).
static const GUID kFilterReferenceGuid = {
    0x7c1f0a53, 0x9b2d, 0x4e77, {0xa3, 0xc8, 0x51, 0xd9, 0xe0, 0xb4, 0xa6, 0xf1}};

//------------------------------------------------------------------------------
// State. The PS3 Eye is one physical device, so one global instance is
// intentional: every open pin shares one timer and one section mapping, and
// each pin tracks its own last delivered frame number so no app starves
// another.
//------------------------------------------------------------------------------

typedef struct _VCAM_PIN_SLOT {
    PKSPIN Pin;                    // NULL while the slot is free
    volatile LONG64 LastFrameNumber;
    BOOLEAN Running;
} VCAM_PIN_SLOT, *PVCAM_PIN_SLOT;

typedef struct _VCAM_STATE {
    KSPIN_LOCK Lock;
    VCAM_PIN_SLOT Pins[VCAM_MAX_PINS];
    ULONG OpenCount;
    ULONG RunCount;
    KTIMER Timer;
    KDPC TimerDpc;
    HANDLE SectionHandle;
    PVOID SectionObject;           // referenced object, not a handle
    PKEVENT ReaderEvent;           // referenced object, not a handle
    PVOID MappedBase;
    SIZE_T MappedSize;
    ULONG DataOffset;
    ULONG DataSize;
    BOOLEAN TimerActive;
} VCAM_STATE;

static VCAM_STATE g_Vcam;

//------------------------------------------------------------------------------
// Diagnostics
//
// A stream that fails to start gives no error a user-mode tool can read, and
// DbgPrint needs an attached debugger. These counters are written to the
// driver's service key, so after a failed capture attempt
// HKLM\SYSTEM\CurrentControlSet\Services\PS3EyeVCamKernel shows how far the
// client actually got. Values are written at PASSIVE_LEVEL only.
//------------------------------------------------------------------------------

#define VCAM_DIAG_COUNT 16

static const WCHAR *const g_DiagNames[VCAM_DIAG_COUNT] = {
    L"DiagPinCreate",     L"DiagPinCreateFail", L"DiagSetFormat",
    L"DiagSetFormatFail", L"DiagRun",           L"DiagProcess",
    L"DiagSubFormat",     L"DiagFormatSize",    L"DiagLoaded",
    L"DiagOpen",          L"DiagIntersect",     L"DiagWantSpec",
    L"DiagWantSize",      L"DiagIntersectFail", L"DiagConnectStatus",
    L"DiagReaderEventStatus"};

static HANDLE g_DiagKey = NULL;
static LONG g_Diag[VCAM_DIAG_COUNT];

static VOID VcamDiagSet(ULONG Index, ULONG Value) {
    UNICODE_STRING valueName;

    if (g_DiagKey == NULL || Index >= VCAM_DIAG_COUNT ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    RtlInitUnicodeString(&valueName, g_DiagNames[Index]);
    ZwSetValueKey(g_DiagKey, &valueName, 0, REG_DWORD, &Value, sizeof(Value));
}

static VOID VcamDiagBump(ULONG Index) {
    if (Index >= VCAM_DIAG_COUNT) {
        return;
    }
    VcamDiagSet(Index, (ULONG)InterlockedIncrement(&g_Diag[Index]));
}

static DRIVER_DISPATCH *g_OriginalCreate = NULL;

static DRIVER_DISPATCH VcamDeviceCreate;

static NTSTATUS VcamDeviceCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    VcamDiagBump(9);
    if (g_OriginalCreate == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    return g_OriginalCreate(DeviceObject, Irp);
}

// Per-call registry logging used to live here: it built a value name in a fixed
// stack buffer and handed it to RtlInitUnicodeString, which scans for a NUL that
// was never written. That unbounded stack read faulted (bugcheck 0x3B) while a
// client was querying the device, so counters with literal names are all that is
// left. Which KS calls a client makes can be seen with a kernel debugger
// instead.

//------------------------------------------------------------------------------
// Shared memory access (PASSIVE_LEVEL only)
//------------------------------------------------------------------------------

// The frame view must be reachable from every thread context. ZwMapViewOfSection
// maps into the *current* process, so the base is a user-mode address that only
// the app which opened the pin can resolve - and VcamDeliverFrame also runs from
// a timer DPC, in whatever context that DPC lands in. Both attempts at a
// process-local view crashed on precisely that: bugcheck 0x7E in
// VcamDeliverFrame (reading header->FrameNumber) and 0x3B in VcamConnect
// (reading header->Magic) once the pin started running.
//
// MmMapViewInSystemSpace maps the section into the system address space instead,
// which is valid regardless of process context, so the DPC and the client both
// see the same pages. It takes the section object rather than a handle.
static VOID VcamDisconnect(VOID) {
    if (g_Vcam.ReaderEvent != NULL) {
        // Tell the service the last reader is gone so it can park the camera.
        KeResetEvent(g_Vcam.ReaderEvent);
        ObDereferenceObject(g_Vcam.ReaderEvent);
        g_Vcam.ReaderEvent = NULL;
    }
    if (g_Vcam.MappedBase != NULL) {
        MmUnmapViewInSystemSpace(g_Vcam.MappedBase);
        g_Vcam.MappedBase = NULL;
        g_Vcam.MappedSize = 0;
    }
    if (g_Vcam.SectionObject != NULL) {
        ObDereferenceObject(g_Vcam.SectionObject);
        g_Vcam.SectionObject = NULL;
    }
    if (g_Vcam.SectionHandle != NULL) {
        ZwClose(g_Vcam.SectionHandle);
        g_Vcam.SectionHandle = NULL;
    }
}

// PASSIVE_LEVEL. Opens the "reader attached" event if the service has created
// it yet, and (re)signals it. Safe to call repeatedly: the service may start
// after the first pin is already open, and the RUN transition retries.
//
// The handle is resolved to the event object and closed straight away: the
// kernel has KeSetEvent/KeResetEvent but no ZwSetEvent/ZwResetEvent.
static VOID VcamEnsureReaderEvent(VOID) {
    UNICODE_STRING eventName;
    OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;
    HANDLE handle = NULL;
    PVOID eventObject = NULL;

    if (g_Vcam.ReaderEvent != NULL) {
        VcamDiagSet(15, 0);
        KeSetEvent(g_Vcam.ReaderEvent, IO_NO_INCREMENT, FALSE);
        return;
    }

    RtlInitUnicodeString(&eventName, PS3EYE_READER_EVENT_NAME);
    InitializeObjectAttributes(&attributes, &eventName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL,
                               NULL);

    // Recorded as-is so a service that never sees the event can be told apart
    // from a driver that never found it.
    status = ZwOpenEvent(&handle, EVENT_MODIFY_STATE, &attributes);
    VcamDiagSet(15, (ULONG)status);
    if (!NT_SUCCESS(status)) {
        return;
    }

    status = ObReferenceObjectByHandle(handle, EVENT_MODIFY_STATE,
                                       *ExEventObjectType, KernelMode,
                                       &eventObject, NULL);
    ZwClose(handle);
    if (!NT_SUCCESS(status)) {
        return;
    }

    g_Vcam.ReaderEvent = (PKEVENT)eventObject;
    KeSetEvent(g_Vcam.ReaderEvent, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS VcamConnectWorker(VOID) {
    UNICODE_STRING sectionName;
    OBJECT_ATTRIBUTES attributes;
    PVOID sectionObject = NULL;
    SIZE_T viewSize = 0;
    NTSTATUS status;

    if (g_Vcam.MappedBase != NULL) {
        return STATUS_SUCCESS;
    }

    RtlInitUnicodeString(&sectionName, PS3EYE_SECTION_NAME);
    InitializeObjectAttributes(&attributes, &sectionName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL,
                               NULL);

    status = ZwOpenSection(&g_Vcam.SectionHandle, SECTION_MAP_READ, &attributes);
    if (!NT_SUCCESS(status)) {
        g_Vcam.SectionHandle = NULL;
        return status;
    }

    // MmMapViewInSystemSpace wants the section object. MmSectionObjectType is
    // not declared by the WDK headers, so pass NULL for the type - the handle
    // came from ZwOpenSection on this same name, so it is a section by
    // construction.
    status = ObReferenceObjectByHandle(g_Vcam.SectionHandle, SECTION_MAP_READ,
                                       NULL, KernelMode, &sectionObject, NULL);
    if (!NT_SUCCESS(status)) {
        ZwClose(g_Vcam.SectionHandle);
        g_Vcam.SectionHandle = NULL;
        return status;
    }

    // A zero size maps the whole section.
    viewSize = 0;
    status = MmMapViewInSystemSpace(sectionObject, &g_Vcam.MappedBase, &viewSize);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(sectionObject);
        ZwClose(g_Vcam.SectionHandle);
        g_Vcam.SectionHandle = NULL;
        g_Vcam.MappedBase = NULL;
        return status;
    }

    g_Vcam.SectionObject = sectionObject;
    g_Vcam.MappedSize = viewSize;

    // Cache the frame layout; the header is stable for the life of the section.
    if (viewSize >= sizeof(PS3EYE_FRAME_HEADER)) {
        const PS3EYE_FRAME_HEADER *header =
            (const PS3EYE_FRAME_HEADER *)g_Vcam.MappedBase;
        if (header->Magic == PS3EYE_MAGIC && header->DataOffset != 0 &&
            header->DataSize != 0) {
            g_Vcam.DataOffset = header->DataOffset;
            g_Vcam.DataSize = header->DataSize;
        } else {
            g_Vcam.DataOffset = sizeof(PS3EYE_FRAME_HEADER);
            g_Vcam.DataSize = PS3EYE_FRAME_SIZE;
        }
    } else {
        g_Vcam.DataOffset = 0;
        g_Vcam.DataSize = 0;
    }

    return STATUS_SUCCESS;
}

// The result is kept in the service key: when the camera opens but no frames
// arrive, STATUS_OBJECT_NAME_NOT_FOUND here says the service is not creating
// the section, and STATUS_SUCCESS says the frames are simply not being written.
static NTSTATUS VcamConnect(VOID) {
    NTSTATUS status = VcamConnectWorker();

    VcamDiagSet(14, (ULONG)status);
    return status;
}

//------------------------------------------------------------------------------
// Format description
//------------------------------------------------------------------------------

static const KS_DATARANGE_VIDEO g_VideoRange = {
    .DataRange = {
        .FormatSize = sizeof(KS_DATARANGE_VIDEO),
        .Flags = 0,
        .SampleSize = PS3EYE_FRAME_SIZE,
        .Reserved = 0,
        .MajorFormat = {0x73646976, 0x0000, 0x0010,
                        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}},
        .SubFormat = {0xe436eb7d, 0x524f, 0x11ce,
                      {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}},
        .Specifier = {0x05589f80, 0xc356, 0x11ce,
                      {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}},
    },
    .bFixedSizeSamples = TRUE,
    .bTemporalCompression = FALSE,
    .StreamDescriptionFlags = 0,
    .MemoryAllocationFlags = 0,
    .ConfigCaps = {
        .guid = {0x05589f80, 0xc356, 0x11ce,
                 {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}},
        .VideoStandard = 0,
        .InputSize = {PS3EYE_WIDTH, PS3EYE_HEIGHT},
        .MinCroppingSize = {PS3EYE_WIDTH, PS3EYE_HEIGHT},
        .MaxCroppingSize = {PS3EYE_WIDTH, PS3EYE_HEIGHT},
        .CropGranularityX = 1,
        .CropGranularityY = 1,
        .CropAlignX = 0,
        .CropAlignY = 0,
        .MinOutputSize = {PS3EYE_WIDTH, PS3EYE_HEIGHT},
        .MaxOutputSize = {PS3EYE_WIDTH, PS3EYE_HEIGHT},
        .OutputGranularityX = 0,
        .OutputGranularityY = 0,
        .StretchTapsX = 0,
        .StretchTapsY = 0,
        .ShrinkTapsX = 0,
        .ShrinkTapsY = 0,
        .MinFrameInterval = PS3EYE_FRAME_INTERVAL_100NS,
        .MaxFrameInterval = PS3EYE_FRAME_INTERVAL_100NS,
        .MinBitsPerSecond = (LONG)PS3EYE_BITS_PER_SECOND,
        .MaxBitsPerSecond = (LONG)PS3EYE_BITS_PER_SECOND,
    },
    // Required: this is the default format clients are offered.
    .VideoInfoHeader = {
        .rcSource = {0, 0, PS3EYE_WIDTH, PS3EYE_HEIGHT},
        .rcTarget = {0, 0, PS3EYE_WIDTH, PS3EYE_HEIGHT},
        .dwBitRate = PS3EYE_BITS_PER_SECOND,
        .dwBitErrorRate = 0,
        .AvgTimePerFrame = PS3EYE_FRAME_INTERVAL_100NS,
        .bmiHeader = {
            .biSize = sizeof(KS_BITMAPINFOHEADER),
            .biWidth = PS3EYE_WIDTH,
            .biHeight = PS3EYE_HEIGHT,
            .biPlanes = 1,
            .biBitCount = PS3EYE_BYTES_PER_PIXEL * 8,
            .biCompression = KS_BI_RGB,
            .biSizeImage = PS3EYE_FRAME_SIZE,
        },
    },
};

// A capture pin must publish allocator framing. Without it the client (Media
// Foundation through the KS proxy, or DirectShow's KSProxy) cannot create the
// buffers it renders into, and starting the stream fails before a single frame
// is requested - which is what a browser reports as NotReadableError.
//
// The macro takes the memory type as a comma-separated GUID, so it is spelled
// through a helper macro: arguments are split before they are expanded, which
// keeps the commas out of the argument itself.
#define PS3EYE_MEMORY_TYPE_KERNEL_NONPAGED                                    \
    0x4a6d5fc4L, 0x7895, 0x11d1, {0xb0, 0x69, 0x00, 0xa0, 0xc9, 0x06, 0x28, 0x02}

DECLARE_SIMPLE_FRAMING_EX(
    g_PinAllocatorFraming,
    PS3EYE_MEMORY_TYPE_KERNEL_NONPAGED,
    KSALLOCATOR_REQUIREMENTF_SYSTEM_MEMORY |
        KSALLOCATOR_REQUIREMENTF_PREFERENCES_ONLY,
    2,
    0,
    PS3EYE_FRAME_SIZE,
    PS3EYE_FRAME_SIZE);

static const KSPIN_INTERFACE g_PinInterfaces[1] = {
    {STATIC_KSINTERFACESETID_Standard, KSINTERFACE_STANDARD_STREAMING, 0}};

// KSMEDIUMSETID_Standard with an any-instance id: ordinary system memory.
static const KSPIN_MEDIUM g_PinMediums[1] = {
    {STATIC_KSMEDIUMSETID_Standard, KSMEDIUM_TYPE_ANYINSTANCE, 0}};

static const PKSDATARANGE g_PinDataRanges[1] = {&g_VideoRange.DataRange};

//------------------------------------------------------------------------------
// Pin dispatch
//------------------------------------------------------------------------------

static NTSTATUS VcamPinProcess(PKSPIN Pin);

// Returns the slot for a pin, or NULL. The array itself is static, so a
// returned pointer stays valid; only its Pin field can be cleared later.
static PVCAM_PIN_SLOT VcamFindSlot(PKSPIN Pin) {
    PVCAM_PIN_SLOT found = NULL;
    KIRQL oldIrql;
    ULONG i;

    KeAcquireSpinLock(&g_Vcam.Lock, &oldIrql);
    for (i = 0; i < VCAM_MAX_PINS; i++) {
        if (g_Vcam.Pins[i].Pin == Pin) {
            found = &g_Vcam.Pins[i];
            break;
        }
    }
    KeReleaseSpinLock(&g_Vcam.Lock, oldIrql);
    return found;
}

static NTSTATUS VcamPinCreate(PKSPIN Pin, PIRP Irp) {
    NTSTATUS status;
    KIRQL oldIrql;
    ULONG i;
    BOOLEAN first = FALSE;

    UNREFERENCED_PARAMETER(Irp);

    KeAcquireSpinLock(&g_Vcam.Lock, &oldIrql);
    for (i = 0; i < VCAM_MAX_PINS; i++) {
        if (g_Vcam.Pins[i].Pin == NULL) {
            g_Vcam.Pins[i].Pin = Pin;
            g_Vcam.Pins[i].LastFrameNumber = 0;
            g_Vcam.Pins[i].Running = FALSE;
            first = (g_Vcam.OpenCount == 0);
            g_Vcam.OpenCount++;
            break;
        }
    }
    KeReleaseSpinLock(&g_Vcam.Lock, oldIrql);

    if (i == VCAM_MAX_PINS) {
        // Too many apps at once; let the client try again later.
        VcamDiagBump(1);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    VcamDiagBump(0);

    // PASSIVE_LEVEL: the capture service may not exist yet, frames then simply
    // stay black until it does.
    if (first) {
        status = VcamConnect();
        if (!NT_SUCCESS(status)) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
                       "PS3EyeVCam: shared memory not available (0x%08X)\n",
                       status);
        }
    }
    VcamEnsureReaderEvent();

    return STATUS_SUCCESS;
}

static NTSTATUS VcamPinClose(PKSPIN Pin, PIRP Irp) {
    KIRQL oldIrql;
    ULONG i;
    BOOLEAN last = FALSE;
    BOOLEAN lastRunner = FALSE;

    UNREFERENCED_PARAMETER(Irp);

    KeAcquireSpinLock(&g_Vcam.Lock, &oldIrql);
    for (i = 0; i < VCAM_MAX_PINS; i++) {
        if (g_Vcam.Pins[i].Pin == Pin) {
            if (g_Vcam.Pins[i].Running) {
                g_Vcam.Pins[i].Running = FALSE;
                if (g_Vcam.RunCount > 0) {
                    g_Vcam.RunCount--;
                }
                lastRunner = (g_Vcam.RunCount == 0);
            }
            g_Vcam.Pins[i].Pin = NULL;
            if (g_Vcam.OpenCount > 0) {
                g_Vcam.OpenCount--;
            }
            last = (g_Vcam.OpenCount == 0);
            break;
        }
    }
    KeReleaseSpinLock(&g_Vcam.Lock, oldIrql);

    // Cancel first so no new tick is queued, then wait out a DPC that may
    // already be running: it can still be copying into this pin's buffer and
    // reading the mapping we are about to unmap.
    if (lastRunner) {
        KeCancelTimer(&g_Vcam.Timer);
        g_Vcam.TimerActive = FALSE;
    }
    KeFlushQueuedDpcs();

    if (last) {
        VcamDisconnect();
    }

    return STATUS_SUCCESS;
}

static NTSTATUS VcamPinReset(PKSPIN Pin, PIRP Irp) {
    PVCAM_PIN_SLOT slot;

    UNREFERENCED_PARAMETER(Irp);

    slot = VcamFindSlot(Pin);
    if (slot != NULL) {
        slot->LastFrameNumber = 0;
    }
    return STATUS_SUCCESS;
}

// The fourth parameter is the proposed connection format (KSDATAFORMAT-sized,
// not the full data range), the fifth is the attribute list.
static NTSTATUS VcamPinSetDataFormat(PKSPIN Pin, PKSDATAFORMAT OldFormat,
                                    PKSMULTIPLE_ITEM OldAttributeList,
                                    const KSDATARANGE *DataRange,
                                    const KSATTRIBUTE_LIST *AttributeRange) {
    UNREFERENCED_PARAMETER(Pin);
    UNREFERENCED_PARAMETER(OldFormat);
    UNREFERENCED_PARAMETER(OldAttributeList);
    UNREFERENCED_PARAMETER(AttributeRange);

    if (DataRange == NULL) {
        VcamDiagBump(3);
        return STATUS_INVALID_PARAMETER;
    }
    // Recorded even when the format is refused: which subtype the client asked
    // for is the first thing to look at when a stream will not start.
    VcamDiagSet(6, DataRange->SubFormat.Data1);
    VcamDiagSet(7, DataRange->FormatSize);
    // Accept our RGB24 format; anything else must be refused so the client
    // negotiates again instead of receiving mis-labelled buffers.
    if (DataRange->FormatSize < sizeof(KSDATAFORMAT)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!IsEqualGUID(&DataRange->MajorFormat, &kMajorTypeVideo) ||
        !IsEqualGUID(&DataRange->SubFormat, &kSubtypeRgb24)) {
        VcamDiagBump(3);
        return STATUS_INVALID_PARAMETER;
    }

    VcamDiagBump(2);
    return STATUS_SUCCESS;
}

// One timer drives every open pin, so it is started by the first pin that
// enters RUN and stopped by the last one that leaves it.
static NTSTATUS VcamPinSetDeviceState(PKSPIN Pin, KSSTATE ToState,
                                      KSSTATE FromState) {
    KIRQL oldIrql;
    BOOLEAN startTimer = FALSE;
    BOOLEAN stopTimer = FALSE;
    ULONG i;

    if (ToState == KSSTATE_RUN && FromState != KSSTATE_RUN) {
        if (g_Vcam.MappedBase == NULL) {
            VcamConnect();
        }
        VcamEnsureReaderEvent();

        KeAcquireSpinLock(&g_Vcam.Lock, &oldIrql);
        for (i = 0; i < VCAM_MAX_PINS; i++) {
            if (g_Vcam.Pins[i].Pin == Pin) {
                if (!g_Vcam.Pins[i].Running) {
                    g_Vcam.Pins[i].Running = TRUE;
                    g_Vcam.Pins[i].LastFrameNumber = 0;
                    startTimer = (g_Vcam.RunCount == 0);
                    g_Vcam.RunCount++;
                }
                break;
            }
        }
        KeReleaseSpinLock(&g_Vcam.Lock, oldIrql);

        VcamDiagBump(4);

        if (startTimer) {
            LARGE_INTEGER dueTime;
            // Relative due time is negative, in 100 ns units; period is in ms.
            dueTime.QuadPart = -((LONGLONG)PS3EYE_FRAME_INTERVAL_100NS);
            KeSetTimerEx(&g_Vcam.Timer, dueTime, PS3EYE_FRAME_INTERVAL_MS,
                         &g_Vcam.TimerDpc);
            g_Vcam.TimerActive = TRUE;
        }
    } else if (FromState == KSSTATE_RUN && ToState != KSSTATE_RUN) {
        KeAcquireSpinLock(&g_Vcam.Lock, &oldIrql);
        for (i = 0; i < VCAM_MAX_PINS; i++) {
            if (g_Vcam.Pins[i].Pin == Pin) {
                if (g_Vcam.Pins[i].Running) {
                    g_Vcam.Pins[i].Running = FALSE;
                    if (g_Vcam.RunCount > 0) {
                        g_Vcam.RunCount--;
                    }
                    stopTimer = (g_Vcam.RunCount == 0);
                }
                break;
            }
        }
        KeReleaseSpinLock(&g_Vcam.Lock, oldIrql);

        if (stopTimer) {
            KeCancelTimer(&g_Vcam.Timer);
            KeFlushQueuedDpcs();
            g_Vcam.TimerActive = FALSE;
        }
    }

    return STATUS_SUCCESS;
}

static const KSPIN_DISPATCH g_PinDispatch = {
    .Create = VcamPinCreate,
    .Close = VcamPinClose,
    .Process = VcamPinProcess,
    .Reset = VcamPinReset,
    .SetDataFormat = VcamPinSetDataFormat,
    .SetDeviceState = VcamPinSetDeviceState,
};

//------------------------------------------------------------------------------
// Frame delivery
//------------------------------------------------------------------------------

static NTSTATUS VcamDeliverFrame(PKSPIN Pin, volatile LONG64 *LastFrameNumber) {
    PKSSTREAM_POINTER leading;
    const PS3EYE_FRAME_HEADER *header;
    ULONGLONG frameNumber;
    ULONG copySize;
    PVOID destination;

    leading = KsPinGetLeadingEdgeStreamPointer(
        Pin, KSSTREAM_POINTER_STATE_LOCKED);
    if (leading == NULL) {
        return STATUS_PENDING;
    }

    if (g_Vcam.MappedBase == NULL || leading->StreamHeader == NULL ||
        leading->StreamHeader->Data == NULL || g_Vcam.MappedSize == 0) {
        KsStreamPointerUnlock(leading, FALSE);
        return STATUS_PENDING;
    }

    header = (const PS3EYE_FRAME_HEADER *)g_Vcam.MappedBase;
    frameNumber = header->FrameNumber;

    // Nothing new since this pin's last delivery, or the header is not ours.
    // The check is per pin: one app consuming frame N must not stop the others
    // from receiving it.
    if (frameNumber == 0 || frameNumber == (ULONGLONG)*LastFrameNumber ||
        header->Magic != PS3EYE_MAGIC) {
        KsStreamPointerUnlock(leading, FALSE);
        return STATUS_PENDING;
    }

    copySize = g_Vcam.DataSize;
    if (copySize == 0 || copySize > PS3EYE_FRAME_SIZE) {
        copySize = PS3EYE_FRAME_SIZE;
    }
    if ((ULONGLONG)g_Vcam.DataOffset + copySize > (ULONGLONG)g_Vcam.MappedSize) {
        KsStreamPointerUnlock(leading, FALSE);
        return STATUS_PENDING;
    }
    if (leading->StreamHeader->FrameExtent < copySize) {
        KsStreamPointerUnlock(leading, FALSE);
        return STATUS_PENDING;
    }

    destination = leading->StreamHeader->Data;
    RtlCopyMemory(destination,
                  (const UCHAR *)g_Vcam.MappedBase + g_Vcam.DataOffset,
                  copySize);

    // If the writer moved on we may have copied a partial frame; the next tick
    // overwrites it, so only adopt the frame number when it looks complete.
    if (header->FrameNumber == frameNumber) {
        *LastFrameNumber = (LONG64)frameNumber;
    }

    {
        LARGE_INTEGER timestamp;
        timestamp.QuadPart = (LONGLONG)header->Timestamp;
        if (timestamp.QuadPart == 0) {
            KeQuerySystemTime(&timestamp);
        }
        leading->StreamHeader->DataUsed = copySize;
        leading->StreamHeader->PresentationTime.Time = timestamp.QuadPart;
        leading->StreamHeader->PresentationTime.Numerator = 1;
        leading->StreamHeader->PresentationTime.Denominator = 1;
        leading->StreamHeader->Duration = PS3EYE_FRAME_INTERVAL_100NS;
        leading->StreamHeader->OptionsFlags |=
            KSSTREAM_HEADER_OPTIONSF_TIMEVALID |
            KSSTREAM_HEADER_OPTIONSF_DURATIONVALID;
    }

    // Advances the output offset by the bytes written, completes the frame when
    // the buffer is full, and releases the lock.
    KsStreamPointerAdvanceOffsetsAndUnlock(leading, 0, copySize, FALSE);
    return STATUS_SUCCESS;
}

static NTSTATUS VcamPinProcess(PKSPIN Pin) {
    PVCAM_PIN_SLOT slot;

    slot = VcamFindSlot(Pin);
    if (slot == NULL) {
        return STATUS_PENDING;
    }
    VcamDiagBump(5);
    return VcamDeliverFrame(Pin, &slot->LastFrameNumber);
}

// DISPATCH_LEVEL. One tick wakes every open pin; the slot array is static and
// AVStream owns each pin until its Close handler returns, and Close flushes the
// DPC queue before that can happen.
static VOID VcamTimerDpc(PKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1,
                         PVOID SystemArgument2) {
    PKSPIN pins[VCAM_MAX_PINS];
    ULONG count = 0;
    ULONG i;
    KIRQL oldIrql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    KeAcquireSpinLock(&g_Vcam.Lock, &oldIrql);
    for (i = 0; i < VCAM_MAX_PINS; i++) {
        if (g_Vcam.Pins[i].Pin != NULL) {
            pins[count++] = g_Vcam.Pins[i].Pin;
        }
    }
    KeReleaseSpinLock(&g_Vcam.Lock, oldIrql);

    for (i = 0; i < count; i++) {
        KsPinAttemptProcessing(pins[i], TRUE);
    }
}

//------------------------------------------------------------------------------
// Filter / device descriptors
//------------------------------------------------------------------------------

// KSFILTER_DESCRIPTOR.Categories is a flat array of GUIDs (const GUID *), not
// an array of GUID pointers: as an array of pointers, ks.sys reads the pointer
// table itself as GUIDs and registers junk interfaces.
//
// Same three categories the AVStream capture samples publish: KSCATEGORY_VIDEO
// and KSCATEGORY_CAPTURE are what the DirectShow and KS proxy paths look for,
// KSCATEGORY_VIDEO_CAMERA is what the camera stack and browsers enumerate.
static const GUID g_FilterCategories[3] = {
    {0x6994ad05, 0x93ef, 0x11d0, {0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}},
    // KSCATEGORY_CAPTURE / KSCATEGORY_VIDEO_CAMERA, kept spelled out above this
    // table because the initialiser needs the literal values.
    {0x65e8773d, 0x8f56, 0x11d0, {0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}},
    {0xe5323777, 0xf976, 0x4f5b, {0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44}},
};

// AVStream's default intersection handler returned STATUS_NOT_FOUND for every
// request Media Foundation made, so the browser never got as far as creating a
// pin. This handler answers what a client actually needs: it matches the major
// type, subtype and specifier, and returns the single format this device has.
// Frame size and rate in the request are not used to reject a match - a client
// that asked for something else simply gets 640x480 at 30 fps back, and can
// reject it itself.
static const GUID kSpecifierVideoInfo2 = {
    0x05589f81, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};

static VOID VcamFillFormatHeader(PVOID Data, ULONG FormatSize, BOOLEAN Second) {
    PKS_DATAFORMAT_VIDEOINFOHEADER2 out2 = (PKS_DATAFORMAT_VIDEOINFOHEADER2)Data;
    PKS_DATAFORMAT_VIDEOINFOHEADER out1 = (PKS_DATAFORMAT_VIDEOINFOHEADER)Data;

    RtlZeroMemory(Data, FormatSize);

    if (Second) {
        RtlCopyMemory(&out2->DataFormat, &g_VideoRange.DataRange,
                      sizeof(KSDATAFORMAT));
        out2->DataFormat.Specifier = kSpecifierVideoInfo2;
        out2->DataFormat.FormatSize = FormatSize;
        out2->DataFormat.SampleSize = PS3EYE_FRAME_SIZE;
        out2->VideoInfoHeader2.rcSource.right = PS3EYE_WIDTH;
        out2->VideoInfoHeader2.rcSource.bottom = PS3EYE_HEIGHT;
        out2->VideoInfoHeader2.rcTarget = out2->VideoInfoHeader2.rcSource;
        out2->VideoInfoHeader2.dwBitRate = PS3EYE_BITS_PER_SECOND;
        out2->VideoInfoHeader2.AvgTimePerFrame = PS3EYE_FRAME_INTERVAL_100NS;
        out2->VideoInfoHeader2.dwPictAspectRatioX = PS3EYE_WIDTH;
        out2->VideoInfoHeader2.dwPictAspectRatioY = PS3EYE_HEIGHT;
        RtlCopyMemory(&out2->VideoInfoHeader2.bmiHeader,
                      &g_VideoRange.VideoInfoHeader.bmiHeader,
                      sizeof(KS_BITMAPINFOHEADER));
    } else {
        // bmiHeader sits in a different place in the two structures, so this
        // case is written out separately rather than reusing the code above.
        RtlCopyMemory(&out1->DataFormat, &g_VideoRange.DataRange,
                      sizeof(KSDATAFORMAT));
        out1->DataFormat.FormatSize = FormatSize;
        out1->DataFormat.SampleSize = PS3EYE_FRAME_SIZE;
        // KS_VIDEOINFOHEADER is exactly the VIDEOINFOHEADER the client expects
        // for this specifier.
        RtlCopyMemory(&out1->VideoInfoHeader,
                      &g_VideoRange.VideoInfoHeader,
                      sizeof(KS_VIDEOINFOHEADER));
        out1->VideoInfoHeader.bmiHeader.biSizeImage = PS3EYE_FRAME_SIZE;
    }
}

static NTSTATUS VcamPinIntersect(PVOID Context, PIRP Irp, PKSP_PIN Pin,
                                 PKSDATARANGE DataRange,
                                 PKSDATARANGE MatchingDataRange,
                                 ULONG DataBufferSize, PVOID Data,
                                 PULONG DataSize) {
    ULONG formatSize;
    BOOLEAN second;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Pin);

    if (DataRange == NULL || MatchingDataRange == NULL || DataSize == NULL) {
        VcamDiagBump(13);
        return STATUS_INVALID_PARAMETER;
    }

    VcamDiagBump(10);

    // Recorded so a refused match says what the client asked for. Only fields
    // inside the 64-byte KSDATARANGE are read: anything past it depends on a
    // length the client supplies, and this must not be able to read out of
    // bounds.
    VcamDiagSet(11, DataRange->Specifier.Data1);
    VcamDiagSet(12, DataRange->FormatSize);

    if (!IsEqualGUID(&DataRange->MajorFormat, &kMajorTypeVideo) ||
        !IsEqualGUID(&DataRange->SubFormat, &kSubtypeRgb24)) {
        VcamDiagBump(13);
        return STATUS_NO_MATCH;
    }

    if (IsEqualGUID(&DataRange->Specifier, &kSpecifierVideoInfo2)) {
        second = TRUE;
        formatSize = sizeof(KSDATAFORMAT) + sizeof(KS_VIDEOINFOHEADER2);
    } else if (IsEqualGUID(&DataRange->Specifier,
                           &g_VideoRange.DataRange.Specifier)) {
        second = FALSE;
        formatSize = sizeof(KSDATAFORMAT) + sizeof(KS_VIDEOINFOHEADER);
    } else {
        VcamDiagBump(13);
        return STATUS_NO_MATCH;
    }

    *DataSize = formatSize;
    if (DataBufferSize == 0) {
        // Size-only query.
        return STATUS_BUFFER_OVERFLOW;
    }
    if (Data == NULL) {
        VcamDiagBump(13);
        return STATUS_INVALID_PARAMETER;
    }
    if (DataBufferSize < formatSize) {
        VcamDiagBump(13);
        return STATUS_BUFFER_TOO_SMALL;
    }

    VcamFillFormatHeader(Data, formatSize, second);
    return STATUS_SUCCESS;
}

static const KSPIN_DESCRIPTOR_EX g_PinDescriptors[1] = {
    {
        .Dispatch = &g_PinDispatch,
        .AutomationTable = NULL,
        .PinDescriptor = {
            .InterfacesCount = 1,
            .Interfaces = g_PinInterfaces,
            .MediumsCount = 1,
            .Mediums = g_PinMediums,
            .DataRangesCount = 1,
            .DataRanges = g_PinDataRanges,
            .DataFlow = KSPIN_DATAFLOW_OUT,
            // A capture pin is bidirectional in practice: the client opens it as
            // the renderer side, and KSProxy refuses SINK-only pins on some
            // capture paths.
            .Communication = KSPIN_COMMUNICATION_BOTH,
            .Category = &kPinCategoryCapture,
            .Name = NULL,
        },
        .Flags = KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY,
        .InstancesPossible = VCAM_MAX_PINS,
        // The filter is useless without its only pin, and saying so lets the
        // graph builder know it must instantiate it.
        .InstancesNecessary = 1,
        .AllocatorFraming = &g_PinAllocatorFraming,
        .IntersectHandler = VcamPinIntersect,
    },
};

static const KSFILTER_DESCRIPTOR g_FilterDescriptor = {
    .Dispatch = NULL,
    .AutomationTable = NULL,
    .Version = KSFILTER_DESCRIPTOR_VERSION,
    // No KSFILTER_FLAG_PRIORITIZE_REFERENCEGUID: that flag announces camera
    // profile support, which this driver does not implement. The capture
    // samples leave it clear.
    .Flags = 0,
    .ReferenceGuid = &kFilterReferenceGuid,
    .PinDescriptorsCount = 1,
    .PinDescriptorSize = sizeof(KSPIN_DESCRIPTOR_EX),
    .PinDescriptors = g_PinDescriptors,
    .CategoriesCount = 3,
    .Categories = g_FilterCategories,
    .NodeDescriptorsCount = 0,
    .NodeDescriptorSize = sizeof(KSNODE_DESCRIPTOR),
    .NodeDescriptors = NULL,
    .ConnectionsCount = 0,
    .Connections = NULL,
    .ComponentId = NULL,
};

// AVStream creates one filter factory per entry here when the device starts.
const KSFILTER_DESCRIPTOR *const g_FilterDescriptors[] = {&g_FilterDescriptor};

static NTSTATUS VcamDeviceAdd(PKSDEVICE Device, PIRP Irp) {
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(Irp);

    KeInitializeSpinLock(&g_Vcam.Lock);
    // KeSetTimerEx needs a synchronisation timer; KeInitializeTimer would give
    // a notification timer and the periodic reload would not work.
    KeInitializeTimerEx(&g_Vcam.Timer, SynchronizationTimer);
    KeInitializeDpc(&g_Vcam.TimerDpc, VcamTimerDpc, NULL);
    g_Vcam.TimerActive = FALSE;
    RtlZeroMemory(g_Vcam.Pins, sizeof(g_Vcam.Pins));
    g_Vcam.OpenCount = 0;
    g_Vcam.RunCount = 0;
    g_Vcam.ReaderEvent = NULL;

    return STATUS_SUCCESS;
}

static const KSDEVICE_DISPATCH g_DeviceDispatch = {
    .Add = VcamDeviceAdd,
};

static const KSDEVICE_DESCRIPTOR g_DeviceDescriptor = {
    .Dispatch = &g_DeviceDispatch,
    .FilterDescriptorsCount = 1,
    .FilterDescriptors = g_FilterDescriptors,
    .Version = 0,
};

//------------------------------------------------------------------------------
// Entry point
//------------------------------------------------------------------------------

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    NTSTATUS status;

    RtlZeroMemory(&g_Vcam, sizeof(g_Vcam));

    {
        OBJECT_ATTRIBUTES attributes;
        // RegistryPath already points at this driver's service key.
        InitializeObjectAttributes(&attributes, RegistryPath,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL,
                                   NULL);
        if (!NT_SUCCESS(ZwOpenKey(&g_DiagKey, KEY_READ | KEY_SET_VALUE,
                                  &attributes))) {
            g_DiagKey = NULL;
        } else {
            // Proves the counters below can be written at all, so a key with
            // only this value means nothing ever opened the device.
            VcamDiagSet(8, 1);
        }
    }

    status = KsInitializeDriver(DriverObject, RegistryPath, &g_DeviceDescriptor);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "PS3EyeVCam: KsInitializeDriver failed 0x%08X\n", status);
        return status;
    }

    // Chain the create dispatch AVStream installed so every attempted open of
    // this device is counted, whether or not a pin ever gets created.
    g_OriginalCreate = DriverObject->MajorFunction[IRP_MJ_CREATE];
    DriverObject->MajorFunction[IRP_MJ_CREATE] = VcamDeviceCreate;

    return status;
}
