// PS3EyeCaptureService.cpp
// Windows Service that captures from PS3 Eye and shares via shared memory
// Install: PS3EyeCaptureService.exe --install
// Uninstall: PS3EyeCaptureService.exe --uninstall

#include "PS3EyeSharedMemory.h"
#include "ps3eye.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <vector>

#pragma comment(lib, "advapi32.lib")

#define SERVICE_NAME L"PS3EyeCaptureService"

// Globals
static SERVICE_STATUS g_serviceStatus = {0};
static SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
static std::atomic<bool> g_running(true);

// The service runs in session 0 with no console, and a camera that fails to
// open leaves the driver's pin waiting forever, so note the milestones in a
// file next to the machine's program data.
static void ServiceLog(const char *format, ...) {
  wchar_t programData[MAX_PATH];
  wchar_t folder[MAX_PATH];
  wchar_t path[MAX_PATH];
  char line[512];
  va_list args;
  int length;
  HANDLE file;
  DWORD written;

  if (GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH) == 0)
    return;
  swprintf_s(folder, L"%s\\PS3Eye", programData);
  CreateDirectoryW(folder, nullptr);
  swprintf_s(path, L"%s\\capture.log", folder);

  va_start(args, format);
  length = vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
  va_end(args);
  if (length <= 0)
    return;

  file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                     OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  WriteFile(file, line, (DWORD)length, &written, nullptr);
  CloseHandle(file);
}

void ReportServiceStatus(DWORD state, DWORD exitCode = 0, DWORD waitHint = 0) {
  static DWORD checkPoint = 1;
  g_serviceStatus.dwCurrentState = state;
  g_serviceStatus.dwWin32ExitCode = exitCode;
  g_serviceStatus.dwWaitHint = waitHint;

  if (state == SERVICE_START_PENDING)
    g_serviceStatus.dwControlsAccepted = 0;
  else
    g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

  if (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
    g_serviceStatus.dwCheckPoint = 0;
  else
    g_serviceStatus.dwCheckPoint = checkPoint++;

  SetServiceStatus(g_statusHandle, &g_serviceStatus);
}

void WINAPI ServiceCtrlHandler(DWORD ctrl) {
  if (ctrl == SERVICE_CONTROL_STOP) {
    ReportServiceStatus(SERVICE_STOP_PENDING);
    g_running = false;
  }
}

void CaptureLoop() {
  PS3EyeSharedMemoryServer sharedMemory;
  if (!sharedMemory.Create()) {
    ServiceLog("start: shared memory unavailable\n");
    return;
  }
  ServiceLog("start: shared memory created\n");

  ps3eye::PS3EYECam::PS3EYERef camera = nullptr;
  bool cameraActive = false;
  std::vector<uint8_t> frameBuffer(PS3EYE_FRAME_SIZE);

  LARGE_INTEGER perfFreq, startTime, currentTime;
  QueryPerformanceFrequency(&perfFreq);
  QueryPerformanceCounter(&startTime);

  LARGE_INTEGER rateStart = startTime, rateTime;
  int framesThisSecond = 0;

  int noClientFrames = 0;

  auto initCamera = [&]() -> bool {
    if (cameraActive)
      return true;
    const auto &devices = ps3eye::PS3EYECam::getDevices(true);
    if (devices.empty()) {
      ServiceLog("reader attached but no PS3 Eye found on USB\n");
      return false;
    }
    ServiceLog("reader attached, %d device(s) found\n", (int)devices.size());
    camera = devices[0];
    if (!camera->init(PS3EYE_WIDTH, PS3EYE_HEIGHT, PS3EYE_FPS,
                      ps3eye::PS3EYECam::EOutputFormat::RGB)) {
      ServiceLog("camera init failed\n");
      camera.reset();
      return false;
    }
    ServiceLog("camera started (init ok)\n");
    camera->setAutogain(true);
    camera->setAutoWhiteBalance(true);
    camera->setFlip(false, true);
    camera->start();
    cameraActive = true;
    return true;
  };

  auto stopCamera = [&]() {
    if (!cameraActive)
      return;
    if (camera && camera->isStreaming())
      camera->stop();
    camera.reset();
    cameraActive = false;
    ServiceLog("camera stopped\n");
  };

  while (g_running) {
    // On-demand: wait for clients
    if (!cameraActive) {
      if (sharedMemory.WaitForClients(1000))
        initCamera();
      continue;
    }

    if (!camera || !camera->isStreaming()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    camera->getFrame(frameBuffer.data());
    framesThisSecond++;

    if (sharedMemory.GetFrameNumber() == 0)
      ServiceLog("first frame captured\n");

    // Report the rate the camera actually delivers at. Frames reaching the
    // section at 30 fps while an app shows a still picture point at the reader;
    // a low number here points at the USB side (the PS3 Eye is isochronous, and
    // an emulated host controller can starve it) rather than at the driver.
    QueryPerformanceCounter(&rateTime);
    if (rateTime.QuadPart - rateStart.QuadPart > perfFreq.QuadPart * 2) {
      ServiceLog("capture: %d frames in 2s, frameNumber=%llu, clients=%ld, kernelReader=%d\n",
                 framesThisSecond,
                 (unsigned long long)sharedMemory.GetFrameNumber(),
                 sharedMemory.GetClientCount(),
                 sharedMemory.HasKernelReader() ? 1 : 0);
      framesThisSecond = 0;
      rateStart = rateTime;
    }

    QueryPerformanceCounter(&currentTime);
    UINT64 timestamp =
        ((currentTime.QuadPart - startTime.QuadPart) * 10000000) /
        perfFreq.QuadPart;
    sharedMemory.WriteFrame(frameBuffer.data(), PS3EYE_FRAME_SIZE, timestamp);

    // Check clients. The kernel camera driver is a reader too, but it cannot
    // bump the user-mode client count, so it is polled separately.
    if (sharedMemory.GetClientCount() <= 0 && !sharedMemory.HasKernelReader()) {
      if (++noClientFrames > 30) {
        stopCamera();
        noClientFrames = 0;
      }
    } else {
      noClientFrames = 0;
    }
  }

  stopCamera();
  sharedMemory.Close();
}

void WINAPI ServiceMain(DWORD argc, LPWSTR *argv) {
  g_statusHandle =
      RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
  if (!g_statusHandle)
    return;

  g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_serviceStatus.dwServiceSpecificExitCode = 0;

  ReportServiceStatus(SERVICE_START_PENDING);
  ReportServiceStatus(SERVICE_RUNNING);

  CaptureLoop();

  ReportServiceStatus(SERVICE_STOPPED);
}

bool InstallService() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);

  // The service is installed under Program Files, so the path must be quoted:
  // an unquoted "C:\Program Files\..." makes the SCM fail to start the service.
  wchar_t binaryPath[MAX_PATH + 2];
  swprintf_s(binaryPath, L"\"%s\"", path);

  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (!scm)
    return false;

  SC_HANDLE svc = CreateServiceW(scm, SERVICE_NAME, L"PS3 Eye Capture Service",
                                 SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                 SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                 binaryPath, nullptr, nullptr, nullptr, nullptr,
                                 nullptr);

  if (!svc) {
    CloseServiceHandle(scm);
    return false;
  }

  // Start immediately
  StartServiceW(svc, 0, nullptr);

  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return true;
}

bool UninstallService() {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm)
    return false;

  SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_STOP | DELETE);
  if (!svc) {
    CloseServiceHandle(scm);
    return false;
  }

  SERVICE_STATUS status;
  ControlService(svc, SERVICE_CONTROL_STOP, &status);
  Sleep(1000);

  DeleteService(svc);
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return true;
}

int wmain(int argc, wchar_t *argv[]) {
  if (argc > 1) {
    if (wcscmp(argv[1], L"--install") == 0 || wcscmp(argv[1], L"-i") == 0) {
      if (InstallService()) {
        wprintf(L"Service installed and started.\n");
        return 0;
      } else {
        wprintf(L"Failed to install. Run as Administrator.\n");
        return 1;
      }
    }
    if (wcscmp(argv[1], L"--uninstall") == 0 || wcscmp(argv[1], L"-u") == 0) {
      if (UninstallService()) {
        wprintf(L"Service uninstalled.\n");
        return 0;
      } else {
        wprintf(L"Failed to uninstall. Run as Administrator.\n");
        return 1;
      }
    }
  }

  // Run as service
  SERVICE_TABLE_ENTRYW serviceTable[] = {{(LPWSTR)SERVICE_NAME, ServiceMain},
                                         {nullptr, nullptr}};

  if (!StartServiceCtrlDispatcherW(serviceTable)) {
    // Not started as service - run directly for testing
    g_running = true;
    CaptureLoop();
  }

  return 0;
}
