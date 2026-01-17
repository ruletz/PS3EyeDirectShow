// PS3EyeMediaSource.cpp
// Media Foundation source implementation for PS3 Eye Camera
// Enables Windows Camera Frame Server sharing for multi-app access

#include "PS3EyeMediaSource.h"
#include <initguid.h> // Must come before any DEFINE_GUID usage
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <ole2.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

// Define the CLSID for PS3Eye Media Source
// {E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}
extern "C" const GUID CLSID_PS3EyeMediaSource = {
    0xe2f5a3d1,
    0x8c7b,
    0x4a2e,
    {0x9f, 0x1d, 0x3b, 0x5c, 0x6d, 0x8e, 0x9a, 0x0b}};

// Helper macro for safe release
#define SAFE_RELEASE(p)                                                        \
  {                                                                            \
    if (p) {                                                                   \
      (p)->Release();                                                          \
      (p) = nullptr;                                                           \
    }                                                                          \
  }

// MF_DEVICESTREAM_FRAMESERVER_SHARED is already defined in mfidl.h (Windows
// 10+) It enables multi-app camera access via Windows Camera Frame Server

//------------------------------------------------------------------------------
// PS3EyeMediaSource Implementation
//------------------------------------------------------------------------------

PS3EyeMediaSource::PS3EyeMediaSource()
    : m_refCount(1), m_state(SourceState::Invalid),
      m_captureThreadRunning(false), m_width(640), m_height(480),
      m_frameRate(30) {}

PS3EyeMediaSource::~PS3EyeMediaSource() { Shutdown(); }

HRESULT PS3EyeMediaSource::CreateInstance(IMFMediaSource **ppSource) {
  if (!ppSource)
    return E_POINTER;

  *ppSource = nullptr;

  PS3EyeMediaSource *pSource = new (std::nothrow) PS3EyeMediaSource();
  if (!pSource)
    return E_OUTOFMEMORY;

  HRESULT hr = pSource->Initialize();
  if (SUCCEEDED(hr)) {
    hr = pSource->QueryInterface(IID_PPV_ARGS(ppSource));
  }

  pSource->Release();
  return hr;
}

HRESULT PS3EyeMediaSource::Initialize() {
  std::lock_guard<std::mutex> lock(m_mutex);

  // Create event queue
  HRESULT hr = MFCreateEventQueue(&m_eventQueue);
  if (FAILED(hr))
    return hr;

  // Create source attributes
  hr = MFCreateAttributes(&m_sourceAttributes, 10);
  if (FAILED(hr))
    return hr;

  // *** CRITICAL: Enable Frame Server sharing ***
  // This attribute tells Windows that this source supports multi-client access
  hr = m_sourceAttributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
  if (FAILED(hr))
    return hr;

  // Set other source attributes
  hr = m_sourceAttributes->SetGUID(
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr))
    return hr;

  // Connect to shared memory (requires PS3EyeCaptureService to be running)
  if (!m_sharedMemClient.Connect()) {
    OutputDebugStringW(
        L"PS3EyeMediaSource: Cannot connect to shared memory.\n");
    OutputDebugStringW(
        L"PS3EyeMediaSource: Make sure PS3EyeCaptureService.exe is running.\n");
    return E_FAIL;
  }
  OutputDebugStringW(L"PS3EyeMediaSource: Connected to shared memory\n");

  // Create the stream
  hr = CreateStream();
  if (FAILED(hr))
    return hr;

  // Create presentation descriptor
  hr = CreatePresentationDescriptorInternal();
  if (FAILED(hr))
    return hr;

  m_state = SourceState::Stopped;
  return S_OK;
}

HRESULT PS3EyeMediaSource::CreateStream() {
  // Create media type for RGB24 video
  ComPtr<IMFMediaType> pMediaType;
  HRESULT hr = MFCreateMediaType(&pMediaType);
  if (FAILED(hr))
    return hr;

  hr = pMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (FAILED(hr))
    return hr;

  hr = pMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB24);
  if (FAILED(hr))
    return hr;

  hr =
      MFSetAttributeSize(pMediaType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
  if (FAILED(hr))
    return hr;

  hr = MFSetAttributeRatio(pMediaType.Get(), MF_MT_FRAME_RATE, m_frameRate, 1);
  if (FAILED(hr))
    return hr;

  hr = MFSetAttributeRatio(pMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (FAILED(hr))
    return hr;

  hr =
      pMediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  if (FAILED(hr))
    return hr;

  // Calculate stride and image size
  LONG stride = m_width * 3; // RGB24 = 3 bytes per pixel
  UINT32 imageSize = stride * m_height;

  hr = pMediaType->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
  if (FAILED(hr))
    return hr;

  hr = pMediaType->SetUINT32(MF_MT_SAMPLE_SIZE, imageSize);
  if (FAILED(hr))
    return hr;

  hr = pMediaType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
  if (FAILED(hr))
    return hr;

  hr = pMediaType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  if (FAILED(hr))
    return hr;

  // Create stream descriptor with this media type
  ComPtr<IMFStreamDescriptor> pSD;
  IMFMediaType *mediaTypes[] = {pMediaType.Get()};
  hr = MFCreateStreamDescriptor(0, 1, mediaTypes, &pSD);
  if (FAILED(hr))
    return hr;

  // Set stream attributes for Frame Server sharing
  ComPtr<IMFAttributes> pStreamAttrs;
  hr = pSD->GetMediaTypeHandler(nullptr); // Just validate

  ComPtr<IMFMediaTypeHandler> pHandler;
  hr = pSD->GetMediaTypeHandler(&pHandler);
  if (SUCCEEDED(hr)) {
    hr = pHandler->SetCurrentMediaType(pMediaType.Get());
  }

  // Create the stream object
  m_stream = new PS3EyeMediaStream(this, pSD.Get());
  if (!m_stream)
    return E_OUTOFMEMORY;

  return S_OK;
}

HRESULT PS3EyeMediaSource::CreatePresentationDescriptorInternal() {
  ComPtr<IMFStreamDescriptor> pSD;
  HRESULT hr = m_stream->GetStreamDescriptor(&pSD);
  if (FAILED(hr))
    return hr;

  IMFStreamDescriptor *streams[] = {pSD.Get()};
  hr = MFCreatePresentationDescriptor(1, streams, &m_presentationDescriptor);
  if (FAILED(hr))
    return hr;

  // Select the first (and only) stream
  hr = m_presentationDescriptor->SelectStream(0);
  return hr;
}

// IUnknown implementation
STDMETHODIMP PS3EyeMediaSource::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv)
    return E_POINTER;

  if (riid == IID_IUnknown)
    *ppv = static_cast<IUnknown *>(static_cast<IMFMediaSourceEx *>(this));
  else if (riid == IID_IMFMediaEventGenerator)
    *ppv = static_cast<IMFMediaEventGenerator *>(this);
  else if (riid == IID_IMFMediaSource)
    *ppv = static_cast<IMFMediaSource *>(this);
  else if (riid == IID_IMFMediaSourceEx)
    *ppv = static_cast<IMFMediaSourceEx *>(this);
  else if (riid == IID_IMFGetService)
    *ppv = static_cast<IMFGetService *>(this);
  else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) PS3EyeMediaSource::AddRef() { return ++m_refCount; }

STDMETHODIMP_(ULONG) PS3EyeMediaSource::Release() {
  ULONG count = --m_refCount;
  if (count == 0)
    delete this;
  return count;
}

// IMFMediaEventGenerator
STDMETHODIMP PS3EyeMediaSource::BeginGetEvent(IMFAsyncCallback *pCallback,
                                              IUnknown *punkState) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == SourceState::Shutdown)
    return MF_E_SHUTDOWN;
  return m_eventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP PS3EyeMediaSource::EndGetEvent(IMFAsyncResult *pResult,
                                            IMFMediaEvent **ppEvent) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == SourceState::Shutdown)
    return MF_E_SHUTDOWN;
  return m_eventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP PS3EyeMediaSource::GetEvent(DWORD dwFlags,
                                         IMFMediaEvent **ppEvent) {
  ComPtr<IMFMediaEventQueue> eventQueue;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state == SourceState::Shutdown)
      return MF_E_SHUTDOWN;
    eventQueue = m_eventQueue;
  }
  return eventQueue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP PS3EyeMediaSource::QueueEvent(MediaEventType met,
                                           REFGUID guidExtendedType,
                                           HRESULT hrStatus,
                                           const PROPVARIANT *pvValue) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == SourceState::Shutdown)
    return MF_E_SHUTDOWN;
  return m_eventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus,
                                          pvValue);
}

// IMFMediaSource
STDMETHODIMP PS3EyeMediaSource::GetCharacteristics(DWORD *pdwCharacteristics) {
  if (!pdwCharacteristics)
    return E_POINTER;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == SourceState::Shutdown)
    return MF_E_SHUTDOWN;

  *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
  return S_OK;
}

STDMETHODIMP PS3EyeMediaSource::CreatePresentationDescriptor(
    IMFPresentationDescriptor **ppPD) {
  if (!ppPD)
    return E_POINTER;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == SourceState::Shutdown)
    return MF_E_SHUTDOWN;

  if (!m_presentationDescriptor)
    return MF_E_NOT_INITIALIZED;

  return m_presentationDescriptor->Clone(ppPD);
}

STDMETHODIMP PS3EyeMediaSource::Start(IMFPresentationDescriptor *pPD,
                                      const GUID *pguidTimeFormat,
                                      const PROPVARIANT *pvarStartPosition) {
  std::lock_guard<std::mutex> lock(m_mutex);

  if (m_state == SourceState::Shutdown)
    return MF_E_SHUTDOWN;

  // Validate time format
  if (pguidTimeFormat && *pguidTimeFormat != GUID_NULL)
    return MF_E_UNSUPPORTED_TIME_FORMAT;

  // Verify shared memory connection
  if (!m_sharedMemClient.IsConnected()) {
    if (!m_sharedMemClient.Connect()) {
      return E_FAIL;
    }
  }

  // Start the stream
  if (m_stream) {
    m_stream->Start();
  }

  // Start capture thread
  StartCaptureThread();

  m_state = SourceState::Started;

  // Queue started event
  PROPVARIANT var;
  PropVariantInit(&var);
  var.vt = VT_EMPTY;
  m_eventQueue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &var);

  return S_OK;
}

STDMETHODIMP PS3EyeMediaSource::Stop() {
  std::lock_guard<std::mutex> lock(m_mutex);

  if (m_state == SourceState::Shutdown)
    return MF_E_SHUTDOWN;

  // Stop capture thread
  StopCaptureThread();

  // Disconnect shared memory
  m_sharedMemClient.Disconnect();

  // Stop stream
  if (m_stream) {
    m_stream->Stop();
  }

  m_state = SourceState::Stopped;

  // Queue stopped event
  return m_eventQueue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK,
                                          nullptr);
}

STDMETHODIMP PS3EyeMediaSource::Pause() {
  std::lock_guard<std::mutex> lock(m_mutex);

  if (m_state == SourceState::Shutdown)
    return MF_E_SHUTDOWN;

  if (m_state != SourceState::Started)
    return MF_E_INVALID_STATE_TRANSITION;

  if (m_stream) {
    m_stream->Pause();
  }

  m_state = SourceState::Paused;

  return m_eventQueue->QueueEventParamVar(MESourcePaused, GUID_NULL, S_OK,
                                          nullptr);
}

STDMETHODIMP PS3EyeMediaSource::Shutdown() {
  std::lock_guard<std::mutex> lock(m_mutex);

  if (m_state == SourceState::Shutdown)
    return S_OK;

  // Stop capture thread first
  StopCaptureThread();

  // Disconnect shared memory
  m_sharedMemClient.Disconnect();

  // Shutdown stream
  if (m_stream) {
    m_stream->Shutdown();
    m_stream.Reset();
  }

  // Shutdown event queue
  if (m_eventQueue) {
    m_eventQueue->Shutdown();
  }

  m_state = SourceState::Shutdown;
  return S_OK;
}

// IMFMediaSourceEx
STDMETHODIMP
PS3EyeMediaSource::GetSourceAttributes(IMFAttributes **ppAttributes) {
  if (!ppAttributes)
    return E_POINTER;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == SourceState::Shutdown)
    return MF_E_SHUTDOWN;

  *ppAttributes = m_sourceAttributes.Get();
  (*ppAttributes)->AddRef();
  return S_OK;
}

STDMETHODIMP
PS3EyeMediaSource::GetStreamAttributes(DWORD dwStreamIdentifier,
                                       IMFAttributes **ppAttributes) {
  if (!ppAttributes)
    return E_POINTER;

  if (dwStreamIdentifier != 0)
    return MF_E_INVALIDSTREAMNUMBER;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == SourceState::Shutdown)
    return MF_E_SHUTDOWN;

  // Create stream attributes with Frame Server sharing enabled
  ComPtr<IMFAttributes> pAttrs;
  HRESULT hr = MFCreateAttributes(&pAttrs, 2);
  if (FAILED(hr))
    return hr;

  // *** CRITICAL: Enable Frame Server sharing on stream level ***
  hr = pAttrs->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
  if (FAILED(hr))
    return hr;

  *ppAttributes = pAttrs.Detach();
  return S_OK;
}

STDMETHODIMP PS3EyeMediaSource::SetD3DManager(IUnknown *pManager) {
  // We don't support D3D acceleration for now
  return S_OK;
}

// IMFGetService
STDMETHODIMP PS3EyeMediaSource::GetService(REFGUID guidService, REFIID riid,
                                           LPVOID *ppvObject) {
  if (!ppvObject)
    return E_POINTER;

  *ppvObject = nullptr;
  return MF_E_UNSUPPORTED_SERVICE;
}

// Capture thread
void PS3EyeMediaSource::StartCaptureThread() {
  if (m_captureThreadRunning)
    return;

  m_captureThreadRunning = true;
  m_captureThread = std::thread(&PS3EyeMediaSource::CaptureThreadProc, this);
}

void PS3EyeMediaSource::StopCaptureThread() {
  m_captureThreadRunning = false;
  if (m_captureThread.joinable()) {
    m_captureThread.join();
  }
}

void PS3EyeMediaSource::CaptureThreadProc() {
  // Allocate frame buffer
  const UINT32 frameSize = m_width * m_height * 3; // RGB24
  std::unique_ptr<uint8_t[]> frameBuffer(new uint8_t[frameSize]);

  LONGLONG timestamp = 0;
  const LONGLONG frameDuration =
      10000000LL / m_frameRate; // 100-nanosecond units

  while (m_captureThreadRunning) {
    // Check shared memory connection
    if (!m_sharedMemClient.IsConnected()) {
      if (!m_sharedMemClient.Connect()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
    }

    // Wait for new frame from shared memory
    if (!m_sharedMemClient.WaitForFrame(100)) {
      continue;
    }

    // Read frame from shared memory (lossless)
    UINT64 frameNum, frameTimestamp;
    if (!m_sharedMemClient.ReadFrame(frameBuffer.get(), frameSize, &frameNum,
                                     &frameTimestamp)) {
      continue;
    }

    // Create MF sample
    ComPtr<IMFSample> pSample;
    HRESULT hr = MFCreateSample(&pSample);
    if (FAILED(hr))
      continue;

    ComPtr<IMFMediaBuffer> pBuffer;
    hr = MFCreateMemoryBuffer(frameSize, &pBuffer);
    if (FAILED(hr))
      continue;

    // Copy frame data to buffer
    BYTE *pDest = nullptr;
    hr = pBuffer->Lock(&pDest, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
      memcpy(pDest, frameBuffer.get(), frameSize);
      pBuffer->Unlock();
      pBuffer->SetCurrentLength(frameSize);
    }

    hr = pSample->AddBuffer(pBuffer.Get());
    if (FAILED(hr))
      continue;

    // Set sample time and duration
    pSample->SetSampleTime(timestamp);
    pSample->SetSampleDuration(frameDuration);
    timestamp += frameDuration;

    // Deliver sample to stream
    if (m_stream) {
      m_stream->DeliverSample(pSample.Get());
    }
  }
}

//------------------------------------------------------------------------------
// PS3EyeMediaStream Implementation
//------------------------------------------------------------------------------

PS3EyeMediaStream::PS3EyeMediaStream(PS3EyeMediaSource *pSource,
                                     IMFStreamDescriptor *pSD)
    : m_refCount(1), m_parent(pSource), m_streamDescriptor(pSD),
      m_isActive(false), m_isShutdown(false) {
  MFCreateEventQueue(&m_eventQueue);
}

PS3EyeMediaStream::~PS3EyeMediaStream() { Shutdown(); }

// IUnknown
STDMETHODIMP PS3EyeMediaStream::QueryInterface(REFIID riid, void **ppv) {
  if (!ppv)
    return E_POINTER;

  if (riid == IID_IUnknown)
    *ppv = static_cast<IUnknown *>(this);
  else if (riid == IID_IMFMediaEventGenerator)
    *ppv = static_cast<IMFMediaEventGenerator *>(this);
  else if (riid == IID_IMFMediaStream)
    *ppv = static_cast<IMFMediaStream *>(this);
  else {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) PS3EyeMediaStream::AddRef() { return ++m_refCount; }

STDMETHODIMP_(ULONG) PS3EyeMediaStream::Release() {
  ULONG count = --m_refCount;
  if (count == 0)
    delete this;
  return count;
}

// IMFMediaEventGenerator
STDMETHODIMP PS3EyeMediaStream::BeginGetEvent(IMFAsyncCallback *pCallback,
                                              IUnknown *punkState) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_isShutdown)
    return MF_E_SHUTDOWN;
  return m_eventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP PS3EyeMediaStream::EndGetEvent(IMFAsyncResult *pResult,
                                            IMFMediaEvent **ppEvent) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_isShutdown)
    return MF_E_SHUTDOWN;
  return m_eventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP PS3EyeMediaStream::GetEvent(DWORD dwFlags,
                                         IMFMediaEvent **ppEvent) {
  ComPtr<IMFMediaEventQueue> eventQueue;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isShutdown)
      return MF_E_SHUTDOWN;
    eventQueue = m_eventQueue;
  }
  return eventQueue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP PS3EyeMediaStream::QueueEvent(MediaEventType met,
                                           REFGUID guidExtendedType,
                                           HRESULT hrStatus,
                                           const PROPVARIANT *pvValue) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_isShutdown)
    return MF_E_SHUTDOWN;
  return m_eventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus,
                                          pvValue);
}

// IMFMediaStream
STDMETHODIMP PS3EyeMediaStream::GetMediaSource(IMFMediaSource **ppMediaSource) {
  if (!ppMediaSource)
    return E_POINTER;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_isShutdown)
    return MF_E_SHUTDOWN;

  *ppMediaSource = m_parent;
  (*ppMediaSource)->AddRef();
  return S_OK;
}

STDMETHODIMP PS3EyeMediaStream::GetStreamDescriptor(
    IMFStreamDescriptor **ppStreamDescriptor) {
  if (!ppStreamDescriptor)
    return E_POINTER;

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_isShutdown)
    return MF_E_SHUTDOWN;

  *ppStreamDescriptor = m_streamDescriptor.Get();
  (*ppStreamDescriptor)->AddRef();
  return S_OK;
}

STDMETHODIMP PS3EyeMediaStream::RequestSample(IUnknown *pToken) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_isShutdown)
    return MF_E_SHUTDOWN;
  if (!m_isActive)
    return MF_E_INVALIDREQUEST;

  // Samples are delivered asynchronously from the capture thread
  // The token is typically used for sample tracking, but we don't need it
  // for this simple implementation
  return S_OK;
}

HRESULT PS3EyeMediaStream::Start() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_isActive = true;
  return m_eventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK,
                                          nullptr);
}

HRESULT PS3EyeMediaStream::Stop() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_isActive = false;
  return m_eventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK,
                                          nullptr);
}

HRESULT PS3EyeMediaStream::Pause() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_isActive = false;
  return m_eventQueue->QueueEventParamVar(MEStreamPaused, GUID_NULL, S_OK,
                                          nullptr);
}

HRESULT PS3EyeMediaStream::Shutdown() {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_isShutdown)
    return S_OK;

  m_isShutdown = true;
  m_isActive = false;

  if (m_eventQueue) {
    m_eventQueue->Shutdown();
  }

  return S_OK;
}

HRESULT PS3EyeMediaStream::DeliverSample(IMFSample *pSample) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_isShutdown)
    return MF_E_SHUTDOWN;
  if (!m_isActive)
    return S_OK; // Silently drop if not active

  // Queue the sample as an event
  return m_eventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK,
                                          pSample);
}
