// PS3EyeVirtualFilter.cpp
// DirectShow Virtual Camera Filter Implementation
// Reads frames from shared memory and provides them as a video source

#include "PS3EyeVirtualFilter.h"
#include <dvdmedia.h>
#include <wmcodecdsp.h>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "winmm.lib")

// Filter registration
const AMOVIESETUP_MEDIATYPE sudOpPinTypes = {&MEDIATYPE_Video,
                                             &MEDIASUBTYPE_RGB24};

const AMOVIESETUP_PIN sudOpPin = {L"Output",
                                  FALSE, // Not rendered
                                  TRUE,  // Output
                                  FALSE, // Can't have zero
                                  FALSE, // Can't have multiple
                                  &CLSID_NULL, nullptr, 1, &sudOpPinTypes};

const AMOVIESETUP_FILTER sudFilter = {&CLSID_PS3EyeVirtualCam, FILTER_NAME,
                                      MERIT_DO_NOT_USE, // Don't auto-insert
                                      1, &sudOpPin};

// Factory template
CFactoryTemplate g_Templates[] = {{FILTER_NAME, &CLSID_PS3EyeVirtualCam,
                                   PS3EyeVirtualCam::CreateInstance, nullptr,
                                   &sudFilter}};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

// DLL exports
STDAPI DllRegisterServer() {
  HRESULT hr = AMovieDllRegisterServer2(TRUE);
  if (FAILED(hr))
    return hr;

  // Register in Video Input Device Category so apps can find us
  IFilterMapper2 *pFM2 = nullptr;
  hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                        IID_IFilterMapper2, (void **)&pFM2);
  if (SUCCEEDED(hr)) {
    REGFILTER2 rf2;
    rf2.dwVersion = 1;
    rf2.dwMerit = MERIT_DO_NOT_USE;
    rf2.cPins = 1;
    rf2.rgPins = &sudOpPin;

    hr = pFM2->RegisterFilter(
        CLSID_PS3EyeVirtualCam, FILTER_NAME, nullptr,
        &CLSID_VideoInputDeviceCategory, // Video capture source category!
        FILTER_NAME, &rf2);

    pFM2->Release();
  }
  return hr;
}

STDAPI DllUnregisterServer() {
  // Unregister from category first
  IFilterMapper2 *pFM2 = nullptr;
  HRESULT hr =
      CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
                       IID_IFilterMapper2, (void **)&pFM2);
  if (SUCCEEDED(hr)) {
    pFM2->UnregisterFilter(&CLSID_VideoInputDeviceCategory, FILTER_NAME,
                           CLSID_PS3EyeVirtualCam);
    pFM2->Release();
  }

  return AMovieDllRegisterServer2(FALSE);
}

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);

BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID lpReserved) {
  return DllEntryPoint(hModule, dwReason, lpReserved);
}

//------------------------------------------------------------------------------
// PS3EyeVirtualCam Implementation
//------------------------------------------------------------------------------

PS3EyeVirtualCam::PS3EyeVirtualCam(LPUNKNOWN lpunk, HRESULT *phr)
    : CSource(NAME("PS3 Eye Virtual Camera"), lpunk, CLSID_PS3EyeVirtualCam) {
  ASSERT(phr);
  CAutoLock cAutoLock(&m_cStateLock);

  // Create output pin
  m_paStreams = (CSourceStream **)new PS3EyeVirtualPin *[1];
  m_paStreams[0] = new PS3EyeVirtualPin(phr, this, L"Video");
}

PS3EyeVirtualCam::~PS3EyeVirtualCam() {
  // Pin is deleted by base class
}

CUnknown *WINAPI PS3EyeVirtualCam::CreateInstance(LPUNKNOWN lpunk,
                                                  HRESULT *phr) {
  ASSERT(phr);
  PS3EyeVirtualCam *pNewFilter = new PS3EyeVirtualCam(lpunk, phr);

  if (pNewFilter == nullptr) {
    if (phr)
      *phr = E_OUTOFMEMORY;
  }

  return pNewFilter;
}

//------------------------------------------------------------------------------
// PS3EyeVirtualPin Implementation
//------------------------------------------------------------------------------

PS3EyeVirtualPin::PS3EyeVirtualPin(HRESULT *phr, PS3EyeVirtualCam *pParent,
                                   LPCWSTR pPinName)
    : CSourceStream(NAME("PS3 Eye Virtual Pin"), phr, pParent, pPinName),
      m_rtLastTime(0), m_lastFrameNumber(0) {}

PS3EyeVirtualPin::~PS3EyeVirtualPin() { m_client.Disconnect(); }

HRESULT PS3EyeVirtualPin::GetMediaType(int iPosition, CMediaType *pmt) {
  CheckPointer(pmt, E_POINTER);
  CAutoLock cAutoLock(m_pFilter->pStateLock());

  if (iPosition < 0)
    return E_INVALIDARG;
  if (iPosition > 0)
    return VFW_S_NO_MORE_ITEMS;

  // Set up RGB24 video format
  VIDEOINFO *pvi = (VIDEOINFO *)pmt->AllocFormatBuffer(sizeof(VIDEOINFO));
  if (pvi == nullptr)
    return E_OUTOFMEMORY;

  ZeroMemory(pvi, sizeof(VIDEOINFO));

  pvi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  pvi->bmiHeader.biWidth = PS3EYE_WIDTH;
  pvi->bmiHeader.biHeight = PS3EYE_HEIGHT; // Positive = bottom-up
  pvi->bmiHeader.biPlanes = 1;
  pvi->bmiHeader.biBitCount = 24;
  pvi->bmiHeader.biCompression = BI_RGB;
  pvi->bmiHeader.biSizeImage = PS3EYE_FRAME_SIZE;

  // Frame timing
  pvi->AvgTimePerFrame = 10000000 / PS3EYE_FPS; // 100ns units

  pmt->SetType(&MEDIATYPE_Video);
  pmt->SetFormatType(&FORMAT_VideoInfo);
  pmt->SetTemporalCompression(FALSE);
  pmt->SetSubtype(&MEDIASUBTYPE_RGB24);
  pmt->SetSampleSize(PS3EYE_FRAME_SIZE);

  return S_OK;
}

HRESULT PS3EyeVirtualPin::CheckMediaType(const CMediaType *pMediaType) {
  CheckPointer(pMediaType, E_POINTER);

  if (*pMediaType->Type() != MEDIATYPE_Video) {
    return E_INVALIDARG;
  }

  if (*pMediaType->Subtype() != MEDIASUBTYPE_RGB24) {
    return E_INVALIDARG;
  }

  if (*pMediaType->FormatType() != FORMAT_VideoInfo) {
    return E_INVALIDARG;
  }

  VIDEOINFO *pvi = (VIDEOINFO *)pMediaType->Format();
  if (pvi == nullptr) {
    return E_INVALIDARG;
  }

  if (pvi->bmiHeader.biWidth != PS3EYE_WIDTH ||
      abs(pvi->bmiHeader.biHeight) != PS3EYE_HEIGHT) {
    return E_INVALIDARG;
  }

  return S_OK;
}

HRESULT PS3EyeVirtualPin::DecideBufferSize(IMemAllocator *pIMemAlloc,
                                           ALLOCATOR_PROPERTIES *pProperties) {
  CheckPointer(pIMemAlloc, E_POINTER);
  CheckPointer(pProperties, E_POINTER);
  CAutoLock cAutoLock(m_pFilter->pStateLock());

  pProperties->cBuffers = 1;
  pProperties->cbBuffer = PS3EYE_FRAME_SIZE;

  ALLOCATOR_PROPERTIES actual;
  HRESULT hr = pIMemAlloc->SetProperties(pProperties, &actual);
  if (FAILED(hr))
    return hr;

  if (actual.cbBuffer < pProperties->cbBuffer) {
    return E_FAIL;
  }

  return S_OK;
}

HRESULT PS3EyeVirtualPin::FillBuffer(IMediaSample *pSample) {
  CheckPointer(pSample, E_POINTER);
  CAutoLock cAutoLock(&m_cSharedState);

  // Connect to shared memory if not connected
  if (!m_client.IsConnected()) {
    if (!m_client.Connect()) {
      // No capture service running - sleep and return
      Sleep(33); // One frame period
      return S_OK;
    }
  }

  // Get buffer pointer
  BYTE *pData;
  HRESULT hr = pSample->GetPointer(&pData);
  if (FAILED(hr))
    return hr;

  // Poll for new frame (with timeout)
  UINT64 frameNumber = 0, timestamp = 0;
  int attempts = 0;
  const int maxAttempts = 10; // ~100ms max wait

  while (attempts < maxAttempts) {
    if (m_client.ReadFrame(pData, PS3EYE_FRAME_SIZE, &frameNumber,
                           &timestamp)) {
      // Got a new frame!
      break;
    }
    // Wait a bit and try again
    Sleep(10);
    attempts++;
  }

  if (attempts >= maxAttempts) {
    // Timeout - no new frame available
    return S_OK;
  }

  pSample->SetActualDataLength(PS3EYE_FRAME_SIZE);

  // Set timestamps
  REFERENCE_TIME rtStart = m_rtLastTime;
  REFERENCE_TIME rtStop = rtStart + (10000000 / PS3EYE_FPS);
  pSample->SetTime(&rtStart, &rtStop);
  m_rtLastTime = rtStop;

  pSample->SetSyncPoint(TRUE);

  return S_OK;
}

STDMETHODIMP PS3EyeVirtualPin::Notify(IBaseFilter *pSender, Quality q) {
  // Quality control - we ignore it for now
  return E_NOTIMPL;
}
