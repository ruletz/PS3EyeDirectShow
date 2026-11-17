// PS3EyeCamInterface.c
//
// The PS3 Eye's capture device (USB\VID_1415&PID_2000&MI_01) is a working KS
// capture device, but its driver only registers KSCATEGORY_CAPTURE. DirectShow
// finds it through that category; Media Foundation - and therefore Chromium and
// the Windows camera stack - only enumerate KSCATEGORY_VIDEO_CAPTURE and
// KSCATEGORY_VIDEO_CAMERA, which nothing on the machine publishes.
//
// This is an upper filter for that device. It forwards every IRP untouched and
// does one thing: registers those two interfaces on the device's PDO, so the
// camera that already streams to DirectShow also shows up in every Media
// Foundation application.
//
// Nothing here touches the USB pipes, the frame stream, or the pin state.

#include <ntddk.h>
#include <ks.h>
#include <ksmedia.h>

// Both categories are written out locally: relying on INITGUID/ksmedia.h
// definitions has already cost more than it is worth in this project.
static const GUID kCategoryVideoCapture = {
    0xcc7bfb41, 0xf175, 0x11d1, {0xa3, 0x92, 0x00, 0xe0, 0x29, 0x1f, 0x39, 0x59}};
static const GUID kCategoryVideoCamera = {
    0xe5323777, 0xf976, 0x4f5b, {0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44}};

#define PS3EYE_FILTER_TAG 'fv3P'

typedef struct _PS3EYE_FILTER_EXTENSION {
    PDEVICE_OBJECT LowerDevice;
    UNICODE_STRING VideoCaptureInterface;
    UNICODE_STRING VideoCameraInterface;
    BOOLEAN InterfacesRegistered;
} PS3EYE_FILTER_EXTENSION, *PPS3EYE_FILTER_EXTENSION;

// ------------------------------------------------------------------- helpers

static VOID Ps3EyeDisableInterfaces(PPS3EYE_FILTER_EXTENSION Extension) {
    if (!Extension->InterfacesRegistered) {
        return;
    }
    // Disabling is best effort: during removal the device may already be gone,
    // and an error here must not stop the detach.
    (VOID) IoSetDeviceInterfaceState(&Extension->VideoCameraInterface, FALSE);
    (VOID) IoSetDeviceInterfaceState(&Extension->VideoCaptureInterface, FALSE);
    Extension->InterfacesRegistered = FALSE;
}

static VOID Ps3EyeFreeInterfaces(PPS3EYE_FILTER_EXTENSION Extension) {
    if (Extension->VideoCaptureInterface.Buffer != NULL) {
        RtlFreeUnicodeString(&Extension->VideoCaptureInterface);
        RtlZeroMemory(&Extension->VideoCaptureInterface, sizeof(UNICODE_STRING));
    }
    if (Extension->VideoCameraInterface.Buffer != NULL) {
        RtlFreeUnicodeString(&Extension->VideoCameraInterface);
        RtlZeroMemory(&Extension->VideoCameraInterface, sizeof(UNICODE_STRING));
    }
}

static NTSTATUS Ps3EyeEnableInterfaces(PPS3EYE_FILTER_EXTENSION Extension) {
    NTSTATUS status;

    if (Extension->VideoCaptureInterface.Buffer == NULL) {
        return STATUS_SUCCESS;
    }

    status = IoSetDeviceInterfaceState(&Extension->VideoCameraInterface, TRUE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = IoSetDeviceInterfaceState(&Extension->VideoCaptureInterface, TRUE);
    if (!NT_SUCCESS(status)) {
        (VOID) IoSetDeviceInterfaceState(&Extension->VideoCameraInterface, FALSE);
        return status;
    }

    Extension->InterfacesRegistered = TRUE;
    return STATUS_SUCCESS;
}

// ------------------------------------------------------------------ dispatch

static NTSTATUS Ps3EyeFilterDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PPS3EYE_FILTER_EXTENSION extension = DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(extension->LowerDevice, Irp);
}

static NTSTATUS Ps3EyeStartComplete(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context) {
    PPS3EYE_FILTER_EXTENSION extension = DeviceObject->DeviceExtension;

    UNREFERENCED_PARAMETER(Context);

    // The interfaces may only be published once the device below has started,
    // otherwise Media Foundation can see a camera it cannot open.
    if (NT_SUCCESS(Irp->IoStatus.Status) && !extension->InterfacesRegistered) {
        (VOID) Ps3EyeEnableInterfaces(extension);
    }

    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Ps3EyeFilterPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    PPS3EYE_FILTER_EXTENSION extension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp, Ps3EyeStartComplete, NULL, TRUE, TRUE, TRUE);
        return IoCallDriver(extension->LowerDevice, Irp);

    case IRP_MN_SURPRISE_REMOVAL:
        // The camera is gone; stop advertising it but keep the stack intact
        // until the remove IRP arrives.
        Ps3EyeDisableInterfaces(extension);
        return Ps3EyeFilterDispatch(DeviceObject, Irp);

    case IRP_MN_REMOVE_DEVICE:
        Ps3EyeDisableInterfaces(extension);
        IoSkipCurrentIrpStackLocation(Irp);
        {
            NTSTATUS status = IoCallDriver(extension->LowerDevice, Irp);
            Ps3EyeFreeInterfaces(extension);
            IoDetachDevice(extension->LowerDevice);
            IoDeleteDevice(DeviceObject);
            // The IRP belongs to the driver below now; it is not touched again.
            return status;
        }

    default:
        return Ps3EyeFilterDispatch(DeviceObject, Irp);
    }
}

static NTSTATUS Ps3EyeFilterPower(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    // Power is the function driver's business. The interfaces stay registered
    // across idle transitions, otherwise the camera would disappear from the
    // enumeration whenever the device went to D3.
    UNREFERENCED_PARAMETER(DeviceObject);
    return Ps3EyeFilterDispatch(DeviceObject, Irp);
}

// -------------------------------------------------------------------- driver

static NTSTATUS Ps3EyeFilterAddDevice(PDRIVER_OBJECT DriverObject,
                                      PDEVICE_OBJECT PhysicalDeviceObject) {
    PDEVICE_OBJECT filter = NULL;
    PPS3EYE_FILTER_EXTENSION extension = NULL;
    NTSTATUS status;

    status = IoCreateDevice(DriverObject, sizeof(PS3EYE_FILTER_EXTENSION), NULL,
                            FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &filter);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    extension = (PPS3EYE_FILTER_EXTENSION) filter->DeviceExtension;
    RtlZeroMemory(extension, sizeof(PS3EYE_FILTER_EXTENSION));

    extension->LowerDevice = IoAttachDeviceToDeviceStack(filter, PhysicalDeviceObject);
    if (extension->LowerDevice == NULL) {
        IoDeleteDevice(filter);
        return STATUS_NO_SUCH_DEVICE;
    }

    filter->Flags |= extension->LowerDevice->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    filter->Characteristics = extension->LowerDevice->Characteristics;
    filter->StackSize = extension->LowerDevice->StackSize + 1;

    // The interfaces belong to the PDO, not to this filter: registering them on
    // the physical device is what makes them survive a filter unload and what
    // lets Media Foundation open the underlying KS device.
    status = IoRegisterDeviceInterface(PhysicalDeviceObject, &kCategoryVideoCapture, NULL,
                                       &extension->VideoCaptureInterface);
    if (!NT_SUCCESS(status)) {
        RtlZeroMemory(&extension->VideoCaptureInterface, sizeof(UNICODE_STRING));
        IoDetachDevice(extension->LowerDevice);
        IoDeleteDevice(filter);
        return status;
    }

    status = IoRegisterDeviceInterface(PhysicalDeviceObject, &kCategoryVideoCamera, NULL,
                                       &extension->VideoCameraInterface);
    if (!NT_SUCCESS(status)) {
        RtlZeroMemory(&extension->VideoCameraInterface, sizeof(UNICODE_STRING));
        Ps3EyeFreeInterfaces(extension);
        IoDetachDevice(extension->LowerDevice);
        IoDeleteDevice(filter);
        return status;
    }

    filter->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    ULONG index;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverExtension->AddDevice = Ps3EyeFilterAddDevice;
    for (index = 0; index <= IRP_MJ_MAXIMUM_FUNCTION; index++) {
        DriverObject->MajorFunction[index] = Ps3EyeFilterDispatch;
    }
    DriverObject->MajorFunction[IRP_MJ_PNP] = Ps3EyeFilterPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = Ps3EyeFilterPower;

    return STATUS_SUCCESS;
}
