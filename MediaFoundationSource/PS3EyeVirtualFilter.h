// PS3EyeVirtualFilter.h
// DirectShow Virtual Camera Filter for PS3 Eye
// Reads from shared memory - works on Windows 10+

#pragma once

// Use the baseclasses from the DirectShowFilter folder
#include "..\DirectShowFilter\baseclasses\streams.h"
#include "PS3EyeSharedMemory.h"
#include <dshow.h>
#include <initguid.h>

// Filter CLSID
// {A1B2C3D4-1234-5678-9ABC-DEF012345678}
DEFINE_GUID(CLSID_PS3EyeVirtualCam, 0xa1b2c3d4, 0x1234, 0x5678, 0x9a, 0xbc,
            0xde, 0xf0, 0x12, 0x34, 0x56, 0x78);

// Filter Name
#define FILTER_NAME L"PS3 Eye Virtual Camera"

//------------------------------------------------------------------------------
// PS3EyeVirtualPin - Output pin that delivers frames
//------------------------------------------------------------------------------
class PS3EyeVirtualCam;

class PS3EyeVirtualPin : public CSourceStream {
public:
  PS3EyeVirtualPin(HRESULT *phr, PS3EyeVirtualCam *pParent, LPCWSTR pPinName);
  virtual ~PS3EyeVirtualPin();

  // CSourceStream overrides
  HRESULT GetMediaType(int iPosition, CMediaType *pmt) override;
  HRESULT CheckMediaType(const CMediaType *pMediaType) override;
  HRESULT DecideBufferSize(IMemAllocator *pIMemAlloc,
                           ALLOCATOR_PROPERTIES *pProperties) override;
  HRESULT FillBuffer(IMediaSample *pSample) override;

  // Quality control
  STDMETHODIMP Notify(IBaseFilter *pSender, Quality q) override;

protected:
  PS3EyeSharedMemoryClient m_client;
  REFERENCE_TIME m_rtLastTime;
  UINT64 m_lastFrameNumber;
  CCritSec m_cSharedState;
};

//------------------------------------------------------------------------------
// PS3EyeVirtualCam - The filter itself
//------------------------------------------------------------------------------
class PS3EyeVirtualCam : public CSource {
public:
  PS3EyeVirtualCam(LPUNKNOWN lpunk, HRESULT *phr);
  virtual ~PS3EyeVirtualCam();

  static CUnknown *WINAPI CreateInstance(LPUNKNOWN lpunk, HRESULT *phr);

  // Optional interfaces
  DECLARE_IUNKNOWN;
};
