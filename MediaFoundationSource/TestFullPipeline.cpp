// TestFullPipeline.cpp
// Tests grabbing actual frames through the DirectShow filter
// Outputs results to a file

#include <dshow.h>
#include <fstream>
#include <initguid.h>
#include <string>
#include <windows.h>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

// Define missing GUIDs
DEFINE_GUID(CLSID_SampleGrabber, 0xC1F400A0, 0x3F08, 0x11d3, 0x9F, 0x0B, 0x00,
            0x60, 0x08, 0x03, 0x9E, 0x37);
DEFINE_GUID(CLSID_NullRenderer, 0xC1F400A4, 0x3F08, 0x11d3, 0x9F, 0x0B, 0x00,
            0x60, 0x08, 0x03, 0x9E, 0x37);
DEFINE_GUID(IID_ISampleGrabber, 0x6B652FFF, 0x11FE, 0x4fce, 0x92, 0xAD, 0x02,
            0x66, 0xB5, 0xD7, 0xC7, 0x8F);
DEFINE_GUID(IID_ISampleGrabberCB, 0x0579154A, 0x2B53, 0x4994, 0xB0, 0xD0, 0xE7,
            0x73, 0x14, 0x8E, 0xFF, 0x85);

// ISampleGrabberCB interface
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
public:
  virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime,
                                             IMediaSample *pSample) = 0;
  virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE *pBuffer,
                                             long BufferLen) = 0;
};

// ISampleGrabber interface
MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
public:
  virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetMediaType(
      const AM_MEDIA_TYPE *pType) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE *
                                                          pType) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long *pBufferSize,
                                                     long *pBuffer) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample *
                                                     *ppSample) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB * pCallback,
                                                long WhichMethodToCallback) = 0;
};

// PS3 Eye Virtual Camera CLSID
static const GUID CLSID_PS3EyeVirtualCam = {
    0xa1b2c3d4,
    0x1234,
    0x5678,
    {0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78}};

std::ofstream g_logFile;

void Log(const char *msg) {
  g_logFile << msg << std::endl;
  g_logFile.flush();
}

void LogHR(const char *msg, HRESULT hr) {
  char buf[512];
  sprintf_s(buf, "%s (hr=0x%08X)", msg, hr);
  Log(buf);
}

// Sample grabber callback
class SampleGrabberCallback : public ISampleGrabberCB {
public:
  LONG m_refCount;
  int m_frameCount;
  DWORD m_firstFrameTime;

  SampleGrabberCallback()
      : m_refCount(1), m_frameCount(0), m_firstFrameTime(0) {}

  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) {
    if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB) {
      *ppv = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_refCount); }
  STDMETHODIMP_(ULONG) Release() {
    LONG ref = InterlockedDecrement(&m_refCount);
    if (ref == 0)
      delete this;
    return ref;
  }

  STDMETHODIMP SampleCB(double time, IMediaSample *pSample) {
    return E_NOTIMPL;
  }

  STDMETHODIMP BufferCB(double time, BYTE *pBuffer, long bufferLen) {
    if (m_frameCount == 0) {
      m_firstFrameTime = GetTickCount();
    }
    m_frameCount++;

    if (m_frameCount <= 5 || m_frameCount % 30 == 0) {
      char msg[256];
      sprintf_s(msg, "  Frame %d: %ld bytes at time %.3f", m_frameCount,
                bufferLen, time);
      Log(msg);
    }
    return S_OK;
  }
};

int main() {
  g_logFile.open("test_pipeline_result.txt");
  Log("=== PS3 Eye Full Pipeline Test ===");
  Log("");

  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hr)) {
    LogHR("ERROR: CoInitialize failed", hr);
    return 1;
  }
  Log("[OK] COM initialized");

  // Create filter graph
  IGraphBuilder *pGraph = NULL;
  hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
                        IID_IGraphBuilder, (void **)&pGraph);
  if (FAILED(hr)) {
    LogHR("ERROR: Cannot create filter graph", hr);
    CoUninitialize();
    return 1;
  }
  Log("[OK] Filter graph created");

  // Create our PS3 Eye filter
  IBaseFilter *pSource = NULL;
  hr = CoCreateInstance(CLSID_PS3EyeVirtualCam, NULL, CLSCTX_INPROC_SERVER,
                        IID_IBaseFilter, (void **)&pSource);
  if (FAILED(hr)) {
    LogHR("ERROR: Cannot create PS3Eye filter", hr);
    pGraph->Release();
    CoUninitialize();
    return 1;
  }
  Log("[OK] PS3 Eye Virtual Camera filter created");

  // Add source to graph
  hr = pGraph->AddFilter(pSource, L"PS3Eye Source");
  if (FAILED(hr)) {
    LogHR("ERROR: Cannot add source to graph", hr);
    pSource->Release();
    pGraph->Release();
    CoUninitialize();
    return 1;
  }
  Log("[OK] Source added to graph");

  // Create sample grabber
  IBaseFilter *pGrabberFilter = NULL;
  hr = CoCreateInstance(CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER,
                        IID_IBaseFilter, (void **)&pGrabberFilter);
  if (FAILED(hr)) {
    LogHR("ERROR: Cannot create sample grabber", hr);
    pSource->Release();
    pGraph->Release();
    CoUninitialize();
    return 1;
  }

  hr = pGraph->AddFilter(pGrabberFilter, L"Sample Grabber");
  if (FAILED(hr)) {
    LogHR("ERROR: Cannot add grabber to graph", hr);
    pGrabberFilter->Release();
    pSource->Release();
    pGraph->Release();
    CoUninitialize();
    return 1;
  }
  Log("[OK] Sample grabber added");

  // Configure sample grabber
  ISampleGrabber *pGrabber = NULL;
  pGrabberFilter->QueryInterface(IID_ISampleGrabber, (void **)&pGrabber);

  AM_MEDIA_TYPE mt;
  ZeroMemory(&mt, sizeof(mt));
  mt.majortype = MEDIATYPE_Video;
  mt.subtype = MEDIASUBTYPE_RGB24;
  pGrabber->SetMediaType(&mt);
  pGrabber->SetBufferSamples(FALSE);
  pGrabber->SetOneShot(FALSE);

  SampleGrabberCallback *callback = new SampleGrabberCallback();
  pGrabber->SetCallback(callback, 1); // BufferCB
  Log("[OK] Sample grabber configured");

  // Create null renderer
  IBaseFilter *pNullRenderer = NULL;
  hr = CoCreateInstance(CLSID_NullRenderer, NULL, CLSCTX_INPROC_SERVER,
                        IID_IBaseFilter, (void **)&pNullRenderer);
  if (FAILED(hr)) {
    LogHR("ERROR: Cannot create null renderer", hr);
  } else {
    pGraph->AddFilter(pNullRenderer, L"Null Renderer");
    Log("[OK] Null renderer added");
  }

  // Connect source to grabber
  IPin *pSourceOut = NULL;
  IEnumPins *pEnum = NULL;
  pSource->EnumPins(&pEnum);
  if (pEnum) {
    pEnum->Next(1, &pSourceOut, NULL);
    pEnum->Release();
  }

  IPin *pGrabberIn = NULL;
  pGrabberFilter->EnumPins(&pEnum);
  if (pEnum) {
    IPin *pPin;
    while (pEnum->Next(1, &pPin, NULL) == S_OK) {
      PIN_DIRECTION dir;
      pPin->QueryDirection(&dir);
      if (dir == PINDIR_INPUT) {
        pGrabberIn = pPin;
        break;
      }
      pPin->Release();
    }
    pEnum->Release();
  }

  if (pSourceOut && pGrabberIn) {
    hr = pGraph->Connect(pSourceOut, pGrabberIn);
    if (FAILED(hr)) {
      LogHR("ERROR: Cannot connect source to grabber", hr);
    } else {
      Log("[OK] Source connected to grabber");
    }
    pSourceOut->Release();
    pGrabberIn->Release();
  }

  // Connect grabber to null renderer
  IPin *pGrabberOut = NULL;
  pGrabberFilter->EnumPins(&pEnum);
  if (pEnum) {
    IPin *pPin;
    while (pEnum->Next(1, &pPin, NULL) == S_OK) {
      PIN_DIRECTION dir;
      pPin->QueryDirection(&dir);
      if (dir == PINDIR_OUTPUT) {
        pGrabberOut = pPin;
        break;
      }
      pPin->Release();
    }
    pEnum->Release();
  }

  IPin *pRendererIn = NULL;
  if (pNullRenderer) {
    pNullRenderer->EnumPins(&pEnum);
    if (pEnum) {
      pEnum->Next(1, &pRendererIn, NULL);
      pEnum->Release();
    }
  }

  if (pGrabberOut && pRendererIn) {
    hr = pGraph->Connect(pGrabberOut, pRendererIn);
    if (FAILED(hr)) {
      LogHR("WARNING: Cannot connect grabber to renderer", hr);
    } else {
      Log("[OK] Grabber connected to renderer");
    }
    pGrabberOut->Release();
    pRendererIn->Release();
  }

  // Run the graph
  IMediaControl *pControl = NULL;
  pGraph->QueryInterface(IID_IMediaControl, (void **)&pControl);

  Log("");
  Log("Starting capture...");
  hr = pControl->Run();
  if (FAILED(hr)) {
    LogHR("ERROR: Cannot run graph", hr);
  } else {
    Log("[OK] Graph running");
    Log("");

    // Wait for frames
    Log("Capturing for 3 seconds...");
    Sleep(3000);

    char msg[256];
    sprintf_s(msg, "\nCaptured %d frames in 3 seconds", callback->m_frameCount);
    Log(msg);

    double fps = callback->m_frameCount / 3.0;
    sprintf_s(msg, "Average FPS: %.1f", fps);
    Log(msg);

    if (callback->m_frameCount > 0) {
      Log("");
      Log("=== TEST PASSED ===");
      Log("Full pipeline is working!");
    } else {
      Log("");
      Log("=== TEST FAILED ===");
      Log("No frames received - check if capture service is running");
    }
  }

  pControl->Stop();

  // Cleanup
  pControl->Release();
  pGrabber->Release();
  callback->Release();
  if (pNullRenderer)
    pNullRenderer->Release();
  pGrabberFilter->Release();
  pSource->Release();
  pGraph->Release();
  CoUninitialize();

  g_logFile.close();
  return 0;
}
