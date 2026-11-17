// PS3EyeVCamRegistrar.cpp
// Creates / removes the PS3 Eye virtual camera through the Windows 11 virtual
// camera API, so Windows (and every Media Foundation app, including browsers)
// enumerates it as a normal camera - no DirectShow flag, no OBS.
//
// Usage (elevated):
//   PS3EyeVCamRegistrar.exe install    create the virtual camera
//   PS3EyeVCamRegistrar.exe remove     remove it again
//   PS3EyeVCamRegistrar.exe status     show camera + capture service state
//
// The media source must already be registered:
//   regsvr32 PS3EyeVCamSource.dll

#include <windows.h>
#include <winsvc.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfvirtualcamera.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#pragma comment(lib, "mf.lib")      // MFEnumDeviceSources (mfidl.h) lives in mf.dll
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfsensorgroup.lib")
#pragma comment(lib, "ole32.lib")

namespace {

// Must match CLSID_PS3EyeMediaSource / PS3EyeVCamSource.cpp. sourceId has to be
// in "{CLSID}" string form.
constexpr wchar_t kSourceClsid[] = L"{E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}";
constexpr wchar_t kCameraName[] = L"PS3 Eye";
constexpr wchar_t kCaptureService[] = L"PS3EyeCaptureService";

void Print(const wchar_t *format, ...) {
  va_list args;
  va_start(args, format);
  vwprintf(format, args);
  va_end(args);
}

// Turns the common failure codes into something actionable.
void Explain(HRESULT hr) {
  Print(L"  HRESULT: 0x%08X\n", static_cast<unsigned>(hr));

  switch (hr) {
  case E_ACCESSDENIED:
    Print(L"  -> Run elevated, and make sure webcam access is allowed for"
          L" desktop apps (Settings > Privacy > Camera).\n");
    break;
  case REGDB_E_CLASSNOTREG:
    Print(L"  -> The media source DLL is not registered. Run as administrator:"
          L" regsvr32 PS3EyeVCamSource.dll\n");
    break;
  case E_INVALIDARG:
    Print(L"  -> Registers sourceId in \"{CLSID}\" format; check that"
          L" kSourceClsid matches CLSID_PS3EyeMediaSource.\n");
    break;
  default:
    if (hr == HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND))
      Print(L"  -> Build/run on Windows 11 (22000+) with the media source DLL"
            L" present.\n");
    break;
  }
}

bool QueryCaptureService(bool *installed, bool *running) {
  *installed = false;
  *running = false;

  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm)
    return false;

  SC_HANDLE service = OpenServiceW(scm, kCaptureService, SERVICE_QUERY_STATUS);
  if (service) {
    *installed = true;
    SERVICE_STATUS status = {};
    if (QueryServiceStatus(service, &status))
      *running = status.dwCurrentState == SERVICE_RUNNING;
    CloseServiceHandle(service);
  }

  CloseServiceHandle(scm);
  return true;
}

HRESULT OpenVirtualCamera(IMFVirtualCamera **camera) {
  // System lifetime keeps the camera available across reboots; AllUsers makes it
  // visible to every account. Pass nullptr categories to register under
  // KSCATEGORY_VIDEO_CAMERA, KSCATEGORY_VIDEO and KSCATEGORY_CAPTURE.
  HRESULT hr = MFCreateVirtualCamera(
      MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
      MFVirtualCameraAccess_AllUsers, kCameraName, kSourceClsid, nullptr, 0,
      camera);

  if (FAILED(hr) && hr != E_ACCESSDENIED) {
    // Retry as the current user only: still useful, avoids a dead end when the
    // shell isn't elevated.
    hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                               MFVirtualCameraLifetime_Session,
                               MFVirtualCameraAccess_CurrentUser, kCameraName,
                               kSourceClsid, nullptr, 0, camera);
    if (SUCCEEDED(hr))
      Print(L"Note: created as a per-user, per-session camera. Run elevated for"
            L" a system-wide camera.\n");
  }

  return hr;
}

int Install() {
  IMFVirtualCamera *camera = nullptr;
  HRESULT hr = OpenVirtualCamera(&camera);
  if (FAILED(hr)) {
    Print(L"Failed to create the virtual camera.\n");
    Explain(hr);
    return 1;
  }

  // nullptr callback: we only need the camera registered, not event callbacks.
  hr = camera->Start(nullptr);
  if (FAILED(hr)) {
    Print(L"Failed to start (register) the virtual camera.\n");
    Explain(hr);
    camera->Release();
    return 1;
  }

  wchar_t link[512] = {};
  UINT32 length = 0;
  if (SUCCEEDED(camera->GetString(
          MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link,
          ARRAYSIZE(link), &length)))
    Print(L"Symbolic link: %s\n", link);

  camera->Release();

  Print(L"Virtual camera installed. It appears in Windows and in apps as\n"
        L"  \"%s Windows Virtual Camera\"\n",
        kCameraName);

  bool installed = false;
  bool running = false;
  if (QueryCaptureService(&installed, &running) && !running) {
    if (installed)
      Print(L"Warning: the %s service is not running, so frames will be black."
            L"\n  Start it with: sc start %s\n",
            kCaptureService, kCaptureService);
    else
      Print(L"Warning: the %s service is not installed, so frames will be"
            L" black.\n  Install it with: PS3EyeCaptureService.exe --install\n",
            kCaptureService);
  }

  Print(L"Test it with any camera app or https://webrtc.github.io/samples/"
        L"src/content/devices/input-output/\n");
  return 0;
}

int Remove() {
  IMFVirtualCamera *camera = nullptr;
  HRESULT hr = OpenVirtualCamera(&camera);
  if (FAILED(hr)) {
    Print(L"Failed to open the virtual camera for removal.\n");
    Explain(hr);
    return 1;
  }

  hr = camera->Remove();
  camera->Release();

  if (FAILED(hr)) {
    Print(L"Failed to remove the virtual camera.\n");
    Explain(hr);
    return 1;
  }

  Print(L"Virtual camera removed.\n");
  return 0;
}

int Status() {
  bool installed = false;
  bool running = false;
  QueryCaptureService(&installed, &running);

  Print(L"Capture service: %s\n",
        !installed ? L"not installed" : (running ? L"running" : L"stopped"));

  IMFAttributes *attributes = nullptr;
  HRESULT hr = MFCreateAttributes(&attributes, 1);
  if (FAILED(hr)) {
    Explain(hr);
    return 1;
  }

  attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

  IMFActivate **devices = nullptr;
  UINT32 count = 0;
  hr = MFEnumDeviceSources(attributes, &devices, &count);
  attributes->Release();

  if (FAILED(hr)) {
    Print(L"Failed to enumerate video capture devices.\n");
    Explain(hr);
    return 1;
  }

  Print(L"Video capture devices (%u):\n", count);
  bool found = false;
  for (UINT32 i = 0; i < count; ++i) {
    wchar_t name[512] = {};
    UINT32 length = 0;
    devices[i]->GetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, name,
                          ARRAYSIZE(name), &length);
    Print(L"  [%u] %s\n", i, name[0] ? name : L"(unnamed)");

    if (std::wstring(name).find(L"PS3 Eye") != std::wstring::npos)
      found = true;

    devices[i]->Release();
  }
  CoTaskMemFree(devices);

  Print(L"\nPS3 Eye virtual camera: %s\n",
        found ? L"visible to Media Foundation apps"
              : L"not enumerated (run \"install\" first)");
  return found ? 0 : 2;
}

} // namespace

int wmain(int argc, wchar_t *argv[]) {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    Print(L"CoInitializeEx failed.\n");
    Explain(hr);
    return 1;
  }

  hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
  if (FAILED(hr)) {
    Print(L"MFStartup failed - is this Windows 11 (build 22000+)?\n");
    Explain(hr);
    CoUninitialize();
    return 1;
  }

  std::wstring command = argc > 1 ? argv[1] : L"status";
  int result = 0;

  if (command == L"install" || command == L"-i")
    result = Install();
  else if (command == L"remove" || command == L"-r" ||
           command == L"uninstall" || command == L"-u")
    result = Remove();
  else if (command == L"status" || command == L"-s")
    result = Status();
  else {
    Print(L"Usage: %s [install|remove|status]\n", argv[0]);
    result = 1;
  }

  MFShutdown();
  CoUninitialize();
  return result;
}
