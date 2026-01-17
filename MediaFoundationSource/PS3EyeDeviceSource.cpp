// PS3EyeDeviceSource.cpp
// Registers PS3Eye as a Windows Camera Frame Server device source
// This is what makes the camera appear in Windows Settings and be shareable

#include "PS3EyeMediaSource.h"
#include <mfapi.h>
#include <mfidl.h>
#include <setupapi.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "setupapi.lib")

// Custom device source activation object
// This is used by Media Foundation to create our source on demand

class PS3EyeActivate : public IMFActivate {
public:
  static HRESULT CreateInstance(IMFActivate **ppActivate);

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // IMFAttributes (inherited by IMFActivate)
  STDMETHODIMP GetItem(REFGUID guidKey, PROPVARIANT *pValue) override;
  STDMETHODIMP GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE *pType) override;
  STDMETHODIMP CompareItem(REFGUID guidKey, REFPROPVARIANT Value,
                           BOOL *pbResult) override;
  STDMETHODIMP Compare(IMFAttributes *pTheirs,
                       MF_ATTRIBUTES_MATCH_TYPE MatchType,
                       BOOL *pbResult) override;
  STDMETHODIMP GetUINT32(REFGUID guidKey, UINT32 *punValue) override;
  STDMETHODIMP GetUINT64(REFGUID guidKey, UINT64 *punValue) override;
  STDMETHODIMP GetDouble(REFGUID guidKey, double *pfValue) override;
  STDMETHODIMP GetGUID(REFGUID guidKey, GUID *pguidValue) override;
  STDMETHODIMP GetStringLength(REFGUID guidKey, UINT32 *pcchLength) override;
  STDMETHODIMP GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize,
                         UINT32 *pcchLength) override;
  STDMETHODIMP GetAllocatedString(REFGUID guidKey, LPWSTR *ppwszValue,
                                  UINT32 *pcchLength) override;
  STDMETHODIMP GetBlobSize(REFGUID guidKey, UINT32 *pcbBlobSize) override;
  STDMETHODIMP GetBlob(REFGUID guidKey, UINT8 *pBuf, UINT32 cbBufSize,
                       UINT32 *pcbBlobSize) override;
  STDMETHODIMP GetAllocatedBlob(REFGUID guidKey, UINT8 **ppBuf,
                                UINT32 *pcbSize) override;
  STDMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID *ppv) override;
  STDMETHODIMP SetItem(REFGUID guidKey, REFPROPVARIANT Value) override;
  STDMETHODIMP DeleteItem(REFGUID guidKey) override;
  STDMETHODIMP DeleteAllItems() override;
  STDMETHODIMP SetUINT32(REFGUID guidKey, UINT32 unValue) override;
  STDMETHODIMP SetUINT64(REFGUID guidKey, UINT64 unValue) override;
  STDMETHODIMP SetDouble(REFGUID guidKey, double fValue) override;
  STDMETHODIMP SetGUID(REFGUID guidKey, REFGUID guidValue) override;
  STDMETHODIMP SetString(REFGUID guidKey, LPCWSTR wszValue) override;
  STDMETHODIMP SetBlob(REFGUID guidKey, const UINT8 *pBuf,
                       UINT32 cbBufSize) override;
  STDMETHODIMP SetUnknown(REFGUID guidKey, IUnknown *pUnknown) override;
  STDMETHODIMP LockStore() override;
  STDMETHODIMP UnlockStore() override;
  STDMETHODIMP GetCount(UINT32 *pcItems) override;
  STDMETHODIMP GetItemByIndex(UINT32 unIndex, GUID *pguidKey,
                              PROPVARIANT *pValue) override;
  STDMETHODIMP CopyAllItems(IMFAttributes *pDest) override;

  // IMFActivate
  STDMETHODIMP ActivateObject(REFIID riid, void **ppv) override;
  STDMETHODIMP ShutdownObject() override;
  STDMETHODIMP DetachObject() override;

private:
  PS3EyeActivate();
  ~PS3EyeActivate();

  std::atomic<ULONG> m_refCount;
  ComPtr<IMFAttributes> m_attributes;
  ComPtr<IMFMediaSource> m_source;
};

PS3EyeActivate::PS3EyeActivate() : m_refCount(1) {
  MFCreateAttributes(&m_attributes, 10);

  // Set device attributes
  m_attributes->SetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                          L"PS3 Eye Camera (Shared)");
  m_attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

  // Symbolic link for device identification
  m_attributes->SetString(
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
      L"\\\\?\\USB#VID_1415&PID_2000#PS3EYE_MF");
}

PS3EyeActivate::~PS3EyeActivate() { ShutdownObject(); }

HRESULT PS3EyeActivate::CreateInstance(IMFActivate **ppActivate) {
  if (!ppActivate)
    return E_POINTER;

  PS3EyeActivate *pActivate = new (std::nothrow) PS3EyeActivate();
  if (!pActivate)
    return E_OUTOFMEMORY;

  *ppActivate = pActivate;
  return S_OK;
}

// IUnknown
STDMETHODIMP PS3EyeActivate::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv)
    return E_POINTER;

  if (riid == IID_IUnknown)
    *ppv = static_cast<IUnknown *>(static_cast<IMFActivate *>(this));
  else if (riid == IID_IMFAttributes)
    *ppv = static_cast<IMFAttributes *>(this);
  else if (riid == IID_IMFActivate)
    *ppv = static_cast<IMFActivate *>(this);
  else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) PS3EyeActivate::AddRef() { return ++m_refCount; }
STDMETHODIMP_(ULONG) PS3EyeActivate::Release() {
  ULONG count = --m_refCount;
  if (count == 0)
    delete this;
  return count;
}

// IMFActivate
STDMETHODIMP PS3EyeActivate::ActivateObject(REFIID riid, void **ppv) {
  if (!ppv)
    return E_POINTER;

  if (!m_source) {
    HRESULT hr = PS3EyeMediaSource::CreateInstance(&m_source);
    if (FAILED(hr))
      return hr;
  }

  return m_source->QueryInterface(riid, ppv);
}

STDMETHODIMP PS3EyeActivate::ShutdownObject() {
  if (m_source) {
    m_source->Shutdown();
    m_source.Reset();
  }
  return S_OK;
}

STDMETHODIMP PS3EyeActivate::DetachObject() {
  m_source.Reset();
  return S_OK;
}

// IMFAttributes - delegate to internal attributes
STDMETHODIMP PS3EyeActivate::GetItem(REFGUID guidKey, PROPVARIANT *pValue) {
  return m_attributes->GetItem(guidKey, pValue);
}
STDMETHODIMP PS3EyeActivate::GetItemType(REFGUID guidKey,
                                         MF_ATTRIBUTE_TYPE *pType) {
  return m_attributes->GetItemType(guidKey, pType);
}
STDMETHODIMP PS3EyeActivate::CompareItem(REFGUID guidKey, REFPROPVARIANT Value,
                                         BOOL *pbResult) {
  return m_attributes->CompareItem(guidKey, Value, pbResult);
}
STDMETHODIMP PS3EyeActivate::Compare(IMFAttributes *pTheirs,
                                     MF_ATTRIBUTES_MATCH_TYPE MatchType,
                                     BOOL *pbResult) {
  return m_attributes->Compare(pTheirs, MatchType, pbResult);
}
STDMETHODIMP PS3EyeActivate::GetUINT32(REFGUID guidKey, UINT32 *punValue) {
  return m_attributes->GetUINT32(guidKey, punValue);
}
STDMETHODIMP PS3EyeActivate::GetUINT64(REFGUID guidKey, UINT64 *punValue) {
  return m_attributes->GetUINT64(guidKey, punValue);
}
STDMETHODIMP PS3EyeActivate::GetDouble(REFGUID guidKey, double *pfValue) {
  return m_attributes->GetDouble(guidKey, pfValue);
}
STDMETHODIMP PS3EyeActivate::GetGUID(REFGUID guidKey, GUID *pguidValue) {
  return m_attributes->GetGUID(guidKey, pguidValue);
}
STDMETHODIMP PS3EyeActivate::GetStringLength(REFGUID guidKey,
                                             UINT32 *pcchLength) {
  return m_attributes->GetStringLength(guidKey, pcchLength);
}
STDMETHODIMP PS3EyeActivate::GetString(REFGUID guidKey, LPWSTR pwszValue,
                                       UINT32 cchBufSize, UINT32 *pcchLength) {
  return m_attributes->GetString(guidKey, pwszValue, cchBufSize, pcchLength);
}
STDMETHODIMP PS3EyeActivate::GetAllocatedString(REFGUID guidKey,
                                                LPWSTR *ppwszValue,
                                                UINT32 *pcchLength) {
  return m_attributes->GetAllocatedString(guidKey, ppwszValue, pcchLength);
}
STDMETHODIMP PS3EyeActivate::GetBlobSize(REFGUID guidKey, UINT32 *pcbBlobSize) {
  return m_attributes->GetBlobSize(guidKey, pcbBlobSize);
}
STDMETHODIMP PS3EyeActivate::GetBlob(REFGUID guidKey, UINT8 *pBuf,
                                     UINT32 cbBufSize, UINT32 *pcbBlobSize) {
  return m_attributes->GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize);
}
STDMETHODIMP PS3EyeActivate::GetAllocatedBlob(REFGUID guidKey, UINT8 **ppBuf,
                                              UINT32 *pcbSize) {
  return m_attributes->GetAllocatedBlob(guidKey, ppBuf, pcbSize);
}
STDMETHODIMP PS3EyeActivate::GetUnknown(REFGUID guidKey, REFIID riid,
                                        LPVOID *ppv) {
  return m_attributes->GetUnknown(guidKey, riid, ppv);
}
STDMETHODIMP PS3EyeActivate::SetItem(REFGUID guidKey, REFPROPVARIANT Value) {
  return m_attributes->SetItem(guidKey, Value);
}
STDMETHODIMP PS3EyeActivate::DeleteItem(REFGUID guidKey) {
  return m_attributes->DeleteItem(guidKey);
}
STDMETHODIMP PS3EyeActivate::DeleteAllItems() {
  return m_attributes->DeleteAllItems();
}
STDMETHODIMP PS3EyeActivate::SetUINT32(REFGUID guidKey, UINT32 unValue) {
  return m_attributes->SetUINT32(guidKey, unValue);
}
STDMETHODIMP PS3EyeActivate::SetUINT64(REFGUID guidKey, UINT64 unValue) {
  return m_attributes->SetUINT64(guidKey, unValue);
}
STDMETHODIMP PS3EyeActivate::SetDouble(REFGUID guidKey, double fValue) {
  return m_attributes->SetDouble(guidKey, fValue);
}
STDMETHODIMP PS3EyeActivate::SetGUID(REFGUID guidKey, REFGUID guidValue) {
  return m_attributes->SetGUID(guidKey, guidValue);
}
STDMETHODIMP PS3EyeActivate::SetString(REFGUID guidKey, LPCWSTR wszValue) {
  return m_attributes->SetString(guidKey, wszValue);
}
STDMETHODIMP PS3EyeActivate::SetBlob(REFGUID guidKey, const UINT8 *pBuf,
                                     UINT32 cbBufSize) {
  return m_attributes->SetBlob(guidKey, pBuf, cbBufSize);
}
STDMETHODIMP PS3EyeActivate::SetUnknown(REFGUID guidKey, IUnknown *pUnknown) {
  return m_attributes->SetUnknown(guidKey, pUnknown);
}
STDMETHODIMP PS3EyeActivate::LockStore() { return m_attributes->LockStore(); }
STDMETHODIMP PS3EyeActivate::UnlockStore() {
  return m_attributes->UnlockStore();
}
STDMETHODIMP PS3EyeActivate::GetCount(UINT32 *pcItems) {
  return m_attributes->GetCount(pcItems);
}
STDMETHODIMP PS3EyeActivate::GetItemByIndex(UINT32 unIndex, GUID *pguidKey,
                                            PROPVARIANT *pValue) {
  return m_attributes->GetItemByIndex(unIndex, pguidKey, pValue);
}
STDMETHODIMP PS3EyeActivate::CopyAllItems(IMFAttributes *pDest) {
  return m_attributes->CopyAllItems(pDest);
}

//------------------------------------------------------------------------------
// DLL Entry Points for COM Registration
//------------------------------------------------------------------------------

// Class factory for PS3EyeActivate
class PS3EyeClassFactory : public IClassFactory {
public:
  // IUnknown
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

  STDMETHODIMP_(ULONG) AddRef() override { return 2; } // Static lifetime
  STDMETHODIMP_(ULONG) Release() override { return 1; }

  // IClassFactory
  STDMETHODIMP CreateInstance(IUnknown *pUnkOuter, REFIID riid,
                              void **ppv) override {
    if (pUnkOuter)
      return CLASS_E_NOAGGREGATION;

    ComPtr<IMFActivate> pActivate;
    HRESULT hr = PS3EyeActivate::CreateInstance(&pActivate);
    if (FAILED(hr))
      return hr;

    return pActivate->QueryInterface(riid, ppv);
  }

  STDMETHODIMP LockServer(BOOL fLock) override { return S_OK; }
};

static PS3EyeClassFactory g_ClassFactory;
static HMODULE g_hModule = nullptr;

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
  if (fdwReason == DLL_PROCESS_ATTACH) {
    g_hModule = hinstDLL;
    DisableThreadLibraryCalls(hinstDLL);
  }
  return TRUE;
}

// Helper to create registry keys
static HRESULT CreateRegistryKey(HKEY hKeyRoot, LPCWSTR subKey,
                                 LPCWSTR valueName, LPCWSTR value) {
  HKEY hKey;
  LONG result =
      RegCreateKeyExW(hKeyRoot, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                      KEY_WRITE, nullptr, &hKey, nullptr);
  if (result != ERROR_SUCCESS) {
    return HRESULT_FROM_WIN32(result);
  }

  if (value != nullptr) {
    result = RegSetValueExW(hKey, valueName, 0, REG_SZ, (const BYTE *)value,
                            (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
  }

  RegCloseKey(hKey);
  return HRESULT_FROM_WIN32(result);
}

// Helper to delete registry keys
static HRESULT DeleteRegistryKey(HKEY hKeyRoot, LPCWSTR subKey) {
  LONG result = RegDeleteTreeW(hKeyRoot, subKey);
  if (result == ERROR_FILE_NOT_FOUND) {
    return S_OK; // Key doesn't exist, that's fine
  }
  return HRESULT_FROM_WIN32(result);
}

// DLL exports
extern "C" {

HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv) {
  if (rclsid == CLSID_PS3EyeMediaSource) {
    return g_ClassFactory.QueryInterface(riid, ppv);
  }
  return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT __stdcall DllCanUnloadNow() {
  return S_FALSE; // Always keep loaded while registered
}

HRESULT __stdcall DllRegisterServer() {
  OutputDebugStringW(
      L"PS3EyeMediaSource: DllRegisterServer - registering COM server\n");

  // Get the DLL path
  WCHAR modulePath[MAX_PATH];
  if (!GetModuleFileNameW(g_hModule, modulePath, MAX_PATH)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }

  HRESULT hr;

  // Register CLSID
  // HKEY_CLASSES_ROOT\CLSID\{E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}
  hr = CreateRegistryKey(HKEY_CLASSES_ROOT,
                         L"CLSID\\{E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}",
                         nullptr, L"PS3 Eye Media Source");
  if (FAILED(hr))
    return hr;

  // Register InprocServer32
  // HKEY_CLASSES_ROOT\CLSID\{E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}\InprocServer32
  hr = CreateRegistryKey(
      HKEY_CLASSES_ROOT,
      L"CLSID\\{E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}\\InprocServer32", nullptr,
      modulePath);
  if (FAILED(hr))
    return hr;

  // Set threading model
  hr = CreateRegistryKey(
      HKEY_CLASSES_ROOT,
      L"CLSID\\{E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}\\InprocServer32",
      L"ThreadingModel", L"Both");
  if (FAILED(hr))
    return hr;

  OutputDebugStringW(L"PS3EyeMediaSource: Registration successful\n");

  // Register as a Media Foundation source
  MFT_REGISTER_TYPE_INFO info = {0};
  info.guidMajorType = MFMediaType_Video;
  info.guidSubtype = MFVideoFormat_RGB24;

  WCHAR name[] = L"PS3 Eye Media Source";
  hr = MFTRegister(CLSID_PS3EyeMediaSource, MFT_CATEGORY_VIDEO_EFFECT, name, 0,
                   0, nullptr, 1, &info, nullptr);

  return hr;
}

HRESULT __stdcall DllUnregisterServer() {
  OutputDebugStringW(
      L"PS3EyeMediaSource: DllUnregisterServer - removing COM server\n");

  // Remove CLSID registration
  HRESULT hr = DeleteRegistryKey(
      HKEY_CLASSES_ROOT, L"CLSID\\{E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}");

  OutputDebugStringW(L"PS3EyeMediaSource: Unregistration complete\n");

  MFTUnregister(CLSID_PS3EyeMediaSource);

  return hr;
}

} // extern "C"
