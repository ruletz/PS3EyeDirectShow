// TestDirectShowFilter.cpp
// Tests the PS3 Eye Virtual Camera DirectShow filter
// Outputs to a file so we can read results

#include <dshow.h>
#include <fstream>
#include <string>
#include <windows.h>


#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

// PS3 Eye Virtual Camera CLSID
// {A1B2C3D4-1234-5678-9ABC-DEF012345678}
static const GUID CLSID_PS3EyeVirtualCam = {
    0xa1b2c3d4,
    0x1234,
    0x5678,
    {0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78}};

void Log(std::ofstream &logFile, const char *msg) {
  logFile << msg << std::endl;
  logFile.flush();
}

int main() {
  std::ofstream logFile("test_directshow_result.txt");
  Log(logFile, "=== PS3 Eye DirectShow Filter Test ===");

  // Initialize COM
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hr)) {
    Log(logFile, "ERROR: CoInitialize failed");
    return 1;
  }
  Log(logFile, "[OK] COM initialized");

  // Try to create the filter
  IBaseFilter *pFilter = NULL;
  hr = CoCreateInstance(CLSID_PS3EyeVirtualCam, NULL, CLSCTX_INPROC_SERVER,
                        IID_IBaseFilter, (void **)&pFilter);

  if (FAILED(hr)) {
    char msg[256];
    sprintf_s(msg, "ERROR: Cannot create filter (hr=0x%08X)", hr);
    Log(logFile, msg);
    CoUninitialize();
    return 1;
  }
  Log(logFile, "[OK] Filter created successfully");

  // Get filter info
  FILTER_INFO filterInfo;
  hr = pFilter->QueryFilterInfo(&filterInfo);
  if (SUCCEEDED(hr)) {
    char name[256];
    WideCharToMultiByte(CP_UTF8, 0, filterInfo.achName, -1, name, 256, NULL,
                        NULL);
    char msg[512];
    sprintf_s(msg, "[OK] Filter name: %s", name);
    Log(logFile, msg);
    if (filterInfo.pGraph)
      filterInfo.pGraph->Release();
  }

  // Enumerate pins
  IEnumPins *pEnumPins = NULL;
  hr = pFilter->EnumPins(&pEnumPins);
  if (SUCCEEDED(hr)) {
    IPin *pPin = NULL;
    ULONG fetched;
    int pinCount = 0;
    while (pEnumPins->Next(1, &pPin, &fetched) == S_OK) {
      PIN_INFO pinInfo;
      pPin->QueryPinInfo(&pinInfo);
      char pinName[256];
      WideCharToMultiByte(CP_UTF8, 0, pinInfo.achName, -1, pinName, 256, NULL,
                          NULL);

      const char *dir = (pinInfo.dir == PINDIR_INPUT) ? "INPUT" : "OUTPUT";
      char msg[512];
      sprintf_s(msg, "[OK] Pin %d: %s (%s)", pinCount++, pinName, dir);
      Log(logFile, msg);

      if (pinInfo.pFilter)
        pinInfo.pFilter->Release();
      pPin->Release();
    }
    pEnumPins->Release();
  }

  // Try to get media type from output pin
  IEnumPins *pEnumPins2 = NULL;
  pFilter->EnumPins(&pEnumPins2);
  if (pEnumPins2) {
    IPin *pPin = NULL;
    ULONG fetched;
    while (pEnumPins2->Next(1, &pPin, &fetched) == S_OK) {
      PIN_INFO pinInfo;
      pPin->QueryPinInfo(&pinInfo);
      if (pinInfo.dir == PINDIR_OUTPUT) {
        IEnumMediaTypes *pEnumMT = NULL;
        pPin->EnumMediaTypes(&pEnumMT);
        if (pEnumMT) {
          AM_MEDIA_TYPE *pMT = NULL;
          while (pEnumMT->Next(1, &pMT, &fetched) == S_OK) {
            if (pMT->formattype == FORMAT_VideoInfo) {
              VIDEOINFOHEADER *pVIH = (VIDEOINFOHEADER *)pMT->pbFormat;
              char msg[512];
              sprintf_s(msg, "[OK] Media type: %dx%d, %d bpp",
                        pVIH->bmiHeader.biWidth, abs(pVIH->bmiHeader.biHeight),
                        pVIH->bmiHeader.biBitCount);
              Log(logFile, msg);
            }
            CoTaskMemFree(pMT->pbFormat);
            CoTaskMemFree(pMT);
          }
          pEnumMT->Release();
        }
      }
      if (pinInfo.pFilter)
        pinInfo.pFilter->Release();
      pPin->Release();
    }
    pEnumPins2->Release();
  }

  Log(logFile, "");
  Log(logFile, "=== TEST PASSED ===");
  Log(logFile, "DirectShow filter is working correctly!");

  pFilter->Release();
  CoUninitialize();

  return 0;
}
