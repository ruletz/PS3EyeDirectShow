// PS3EyeMediaSource.h
// Media Foundation source for PS3 Eye Camera with Frame Server support
// Enables multi-application camera sharing via Windows Camera Frame Server

#pragma once

// Windows headers must come first
#include <objbase.h>
#include <windows.h>

// Media Foundation
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

// WRL for ComPtr
#include <wrl/client.h>

// Standard library
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

// Shared memory client for reading frames from capture service
#include "PS3EyeSharedMemory.h"

using Microsoft::WRL::ComPtr;

// Forward declare the CLSID (defined in PS3EyeMediaSource.cpp)
// {E2F5A3D1-8C7B-4A2E-9F1D-3B5C6D8E9A0B}
extern "C" const GUID CLSID_PS3EyeMediaSource;

// Forward declarations
class PS3EyeMediaStream;

//------------------------------------------------------------------------------
// PS3EyeMediaSource
// Implements IMFMediaSource for the PS3 Eye camera
// Key feature: Sets MF_DEVICESTREAM_FRAMESERVER_SHARED for multi-app sharing
//------------------------------------------------------------------------------
class PS3EyeMediaSource : public IMFMediaSourceEx, public IMFGetService {
public:
  // Static factory method
  static HRESULT CreateInstance(IMFMediaSource **ppSource);

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // IMFMediaEventGenerator
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback *pCallback,
                             IUnknown *punkState) override;
  STDMETHODIMP EndGetEvent(IMFAsyncResult *pResult,
                           IMFMediaEvent **ppEvent) override;
  STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent) override;
  STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                          HRESULT hrStatus,
                          const PROPVARIANT *pvValue) override;

  // IMFMediaSource
  STDMETHODIMP
  CreatePresentationDescriptor(IMFPresentationDescriptor **ppPD) override;
  STDMETHODIMP GetCharacteristics(DWORD *pdwCharacteristics) override;
  STDMETHODIMP Pause() override;
  STDMETHODIMP Shutdown() override;
  STDMETHODIMP Start(IMFPresentationDescriptor *pPD,
                     const GUID *pguidTimeFormat,
                     const PROPVARIANT *pvarStartPosition) override;
  STDMETHODIMP Stop() override;

  // IMFMediaSourceEx
  STDMETHODIMP GetSourceAttributes(IMFAttributes **ppAttributes) override;
  STDMETHODIMP GetStreamAttributes(DWORD dwStreamIdentifier,
                                   IMFAttributes **ppAttributes) override;
  STDMETHODIMP SetD3DManager(IUnknown *pManager) override;

  // IMFGetService
  STDMETHODIMP GetService(REFGUID guidService, REFIID riid,
                          LPVOID *ppvObject) override;

  // Internal methods
  HRESULT Initialize();
  HRESULT DeliverSample(IMFSample *pSample);

private:
  PS3EyeMediaSource();
  ~PS3EyeMediaSource();

  HRESULT CreateStream();
  HRESULT CreatePresentationDescriptorInternal();
  HRESULT ValidatePresentationDescriptor(IMFPresentationDescriptor *pPD);

  void CaptureThreadProc();
  void StartCaptureThread();
  void StopCaptureThread();

  // Reference count
  std::atomic<ULONG> m_refCount;

  // Critical section for thread safety
  std::mutex m_mutex;

  // Media Foundation objects
  ComPtr<IMFMediaEventQueue> m_eventQueue;
  ComPtr<IMFPresentationDescriptor> m_presentationDescriptor;
  ComPtr<IMFAttributes> m_sourceAttributes;
  ComPtr<PS3EyeMediaStream> m_stream;

  // Shared memory client (reads from PS3EyeCaptureService)
  PS3EyeSharedMemoryClient m_sharedMemClient;

  // State
  enum class SourceState { Invalid, Stopped, Started, Paused, Shutdown };
  SourceState m_state;

  // Capture thread
  std::thread m_captureThread;
  std::atomic<bool> m_captureThreadRunning;

  // Video format
  UINT32 m_width;
  UINT32 m_height;
  UINT32 m_frameRate;
};

//------------------------------------------------------------------------------
// PS3EyeMediaStream
// Implements IMFMediaStream for the PS3 Eye camera
//------------------------------------------------------------------------------
class PS3EyeMediaStream : public IMFMediaStream {
public:
  PS3EyeMediaStream(PS3EyeMediaSource *pSource, IMFStreamDescriptor *pSD);
  virtual ~PS3EyeMediaStream();

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // IMFMediaEventGenerator
  STDMETHODIMP BeginGetEvent(IMFAsyncCallback *pCallback,
                             IUnknown *punkState) override;
  STDMETHODIMP EndGetEvent(IMFAsyncResult *pResult,
                           IMFMediaEvent **ppEvent) override;
  STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent **ppEvent) override;
  STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                          HRESULT hrStatus,
                          const PROPVARIANT *pvValue) override;

  // IMFMediaStream
  STDMETHODIMP GetMediaSource(IMFMediaSource **ppMediaSource) override;
  STDMETHODIMP
  GetStreamDescriptor(IMFStreamDescriptor **ppStreamDescriptor) override;
  STDMETHODIMP RequestSample(IUnknown *pToken) override;

  // Internal methods
  HRESULT Start();
  HRESULT Stop();
  HRESULT Pause();
  HRESULT Shutdown();
  HRESULT DeliverSample(IMFSample *pSample);

private:
  std::atomic<ULONG> m_refCount;
  std::mutex m_mutex;

  PS3EyeMediaSource *m_parent; // Weak reference
  ComPtr<IMFStreamDescriptor> m_streamDescriptor;
  ComPtr<IMFMediaEventQueue> m_eventQueue;

  bool m_isActive;
  bool m_isShutdown;
};
