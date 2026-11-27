// PS3EyeVCamSource.cpp
// COM entry points for the PS3 Eye Frame Server custom media source.
//
// This DLL is what the Windows Camera Frame Server loads (Local Service, session
// 0) once a virtual camera was created with MFCreateVirtualCamera() using
// CLSID_PS3EyeMediaSource as the "sourceId". It only exposes the media source
// that already lives in MediaFoundationSource/PS3EyeMediaSource.cpp; all frame
// production happens in PS3EyeCaptureService.exe via shared memory.
//
// Requires Windows 11 build 22000 or newer (virtual camera API).

#include "PS3EyeMediaSource.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <ole2.h>

#include <cwchar>
#include <mutex>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace {

HMODULE g_hModule = nullptr;

// Media Foundation is reference counted, so starting it here is safe even when
// the host process already did so.
std::mutex g_mfMutex;
bool g_mfStarted = false;

HRESULT EnsureMediaFoundation() {
  std::lock_guard<std::mutex> lock(g_mfMutex);
  if (g_mfStarted)
    return S_OK;

  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (SUCCEEDED(hr))
    g_mfStarted = true;
  return hr;
}

// CLSID written into the registry and passed to MFCreateVirtualCamera() as
// sourceId. Must stay in sync with CLSID_PS3EyeMediaSource.
constexpr wchar_t kClsidString[] = L"{E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}";
constexpr wchar_t kClsidKey[] =
    L"SOFTWARE\\Classes\\CLSID\\{E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}";
constexpr wchar_t kSourceName[] = L"PS3 Eye Capture Source";

class PS3EyeSourceClassFactory : public IClassFactory {
public:
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv)
      return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *ppv = static_cast<IClassFactory *>(this);
      AddRef();
      return S_OK;
    }

    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  // Static lifetime: the factory lives as long as the DLL is loaded.
  STDMETHODIMP_(ULONG) AddRef() override { return 2; }
  STDMETHODIMP_(ULONG) Release() override { return 1; }

  STDMETHODIMP CreateInstance(IUnknown *pUnkOuter, REFIID riid,
                              void **ppv) override {
    if (!ppv)
      return E_POINTER;
    *ppv = nullptr;

    if (pUnkOuter)
      return CLASS_E_NOAGGREGATION;

    HRESULT hr = EnsureMediaFoundation();
    if (FAILED(hr))
      return hr;

    Microsoft::WRL::ComPtr<IMFMediaSource> source;
    hr = PS3EyeMediaSource::CreateInstance(&source);
    if (FAILED(hr))
      return hr;

    return source->QueryInterface(riid, ppv);
  }

  STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};

PS3EyeSourceClassFactory g_classFactory;

// Writes a string value, creating the key when needed.
HRESULT SetRegString(HKEY root, LPCWSTR subKey, LPCWSTR valueName,
                     LPCWSTR value) {
  HKEY key = nullptr;
  LONG result = RegCreateKeyExW(root, subKey, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                &key, nullptr);
  if (result != ERROR_SUCCESS)
    return HRESULT_FROM_WIN32(result);

  result = RegSetValueExW(key, valueName, 0, REG_SZ,
                          reinterpret_cast<const BYTE *>(value),
                          static_cast<DWORD>((wcslen(value) + 1) *
                                             sizeof(wchar_t)));
  RegCloseKey(key);
  return HRESULT_FROM_WIN32(result);
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
  if (fdwReason == DLL_PROCESS_ATTACH) {
    g_hModule = hinstDLL;
    DisableThreadLibraryCalls(hinstDLL);
  }
  return TRUE;
}

extern "C" {

HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv) {
  if (!ppv)
    return E_POINTER;

  if (rclsid != CLSID_PS3EyeMediaSource)
    return CLASS_E_CLASSNOTAVAILABLE;

  return g_classFactory.QueryInterface(riid, ppv);
}

HRESULT __stdcall DllCanUnloadNow() {
  // The factory has a static lifetime and media sources are held by the frame
  // server, so never unload while registered.
  return S_FALSE;
}

// Must be run elevated: the frame server loads the source through an HKLM
// registration, not HKCU.
HRESULT __stdcall DllRegisterServer() {
  wchar_t modulePath[MAX_PATH] = {};
  if (!GetModuleFileNameW(g_hModule, modulePath, MAX_PATH))
    return HRESULT_FROM_WIN32(GetLastError());

  HRESULT hr = SetRegString(HKEY_LOCAL_MACHINE, kClsidKey, nullptr,
                            kSourceName);
  if (FAILED(hr))
    return hr;

  std::wstring inprocKey = std::wstring(kClsidKey) + L"\\InprocServer32";

  hr = SetRegString(HKEY_LOCAL_MACHINE, inprocKey.c_str(), nullptr, modulePath);
  if (FAILED(hr))
    return hr;

  return SetRegString(HKEY_LOCAL_MACHINE, inprocKey.c_str(), L"ThreadingModel",
                      L"Both");
}

HRESULT __stdcall DllUnregisterServer() {
  LONG result = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey);
  if (result == ERROR_FILE_NOT_FOUND)
    return S_OK;
  return HRESULT_FROM_WIN32(result);
}

} // extern "C"
