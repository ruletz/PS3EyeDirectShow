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

// Watchdog state. `getFrame` waits for a frame with no timeout, so a stream that
// dies (cable pulled, camera wedged) parks the capture thread inside it forever:
// the service then sits in STOP_PENDING and only taskkill clears it. A separate
// thread watches this timestamp and restarts the process instead. It has to exit
// rather than reinitialise, because the camera thread cannot be recovered and
// PS3EYECam::stop() waits on that same stuck thread.
static std::atomic<UINT64> g_lastFrameTaken{0};  // QPC reading, 0 = idle
static std::atomic<int> g_framesSeen(0);
static std::atomic<bool> g_captureActive(false);
static const double kStallSeconds = 10.0;

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

// Test aid. A virtual machine can pass the camera through but its emulated host
// controller never completes the isochronous transfers, which leaves the rest of
// the path (shared section, Media Foundation source, kernel camera) impossible to
// exercise. Setting PS3EYE_SYNTHETIC=1 in the machine environment makes the
// service publish a moving test pattern at the camera's frame rate instead of
// opening the camera, so everything downstream can be verified on its own.
// 0 = off, 1 = publish the pattern, 2 = publish 30 frames and then stop, which
// exercises the stall watchdog without touching the camera or the device.
static int SyntheticMode() {
  wchar_t value[8];
  DWORD length = GetEnvironmentVariableW(L"PS3EYE_SYNTHETIC", value, 8);
  if (length == 0)
    return 0;
  if (value[0] == L'1')
    return 1;
  if (_wcsicmp(value, L"stall") == 0)
    return 2;
  return 0;
}

// Moving gradient plus a scrolling bar: a frozen frame and a moving one look
// different both to a screenshot and to the page's painted counter.
static void FillSyntheticFrame(uint8_t *pixels, UINT32 index) {
  const UINT32 bar = (index * 8) % PS3EYE_WIDTH;
  for (UINT32 y = 0; y < PS3EYE_HEIGHT; ++y) {
    uint8_t *row = pixels + y * PS3EYE_WIDTH * PS3EYE_BYTES_PER_PIXEL;
    for (UINT32 x = 0; x < PS3EYE_WIDTH; ++x) {
      UINT32 dx = (x > bar) ? (x - bar) : (bar - x);
      bool inBar = dx < 40;
      row[x * 3 + 0] = inBar ? 255 : (uint8_t)(x * 255 / PS3EYE_WIDTH);
      row[x * 3 + 1] = inBar ? 255 : (uint8_t)(y * 255 / PS3EYE_HEIGHT);
      row[x * 3 + 2] = inBar ? 0 : (uint8_t)((index * 3) & 0xFF);
    }
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
  const int synthetic = SyntheticMode();
  UINT32 syntheticIndex = 0;
  auto nextTick = std::chrono::steady_clock::now();

  auto noteFrame = [&]() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_lastFrameTaken.store((UINT64)now.QuadPart);
    g_framesSeen.fetch_add(1);
  };

  // Arms the stall clock. Called by both frame sources: a camera that never
  // delivers must be caught too, not just one that stops mid-stream.
  auto armStallClock = [&]() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_framesSeen.store(0);
    g_lastFrameTaken.store((UINT64)now.QuadPart);
    g_captureActive.store(true);
  };

  auto initCamera = [&]() -> bool {
    if (cameraActive)
      return true;
    if (synthetic) {
      ServiceLog("synthetic capture: no camera opened\n");
      cameraActive = true;
      armStallClock();
      return true;
    }
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
    armStallClock();
    camera->setAutogain(true);
    camera->setAutoWhiteBalance(true);
    camera->setFlip(false, true);
    camera->start();
    cameraActive = true;
    return true;
  };

  std::thread watchdog([&]() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    while (g_running) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!g_captureActive.load())
        continue;
      UINT64 last = g_lastFrameTaken.load();
      if (last == 0)
        continue;
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      double idle = (double)((UINT64)now.QuadPart - last) / (double)freq.QuadPart;
      if (idle < kStallSeconds)
        continue;
      ServiceLog("capture stalled %.0f s after %d frames (kernelReader=%d): "
                 "restarting the service process\n",
                 idle, g_framesSeen.load(),
                 sharedMemory.HasKernelReader() ? 1 : 0);
      // The service control manager's recovery actions restart this service
      // (installed with --install); exiting is the only way out of a blocked
      // capture thread.
      ExitProcess(1);
    }
  });

  auto stopCamera = [&]() {
    if (!cameraActive)
      return;
    if (synthetic) {
      cameraActive = false;
      g_captureActive.store(false);
      ServiceLog("synthetic capture stopped\n");
      return;
    }
    if (camera && camera->isStreaming())
      camera->stop();
    camera.reset();
    cameraActive = false;
    g_captureActive.store(false);
    ServiceLog("camera stopped\n");
  };

  while (g_running) {
    // On-demand: wait for clients
    if (!cameraActive) {
      if (sharedMemory.WaitForClients(1000))
        initCamera();
      continue;
    }

    if (!synthetic && (!camera || !camera->isStreaming())) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    if (synthetic) {
      nextTick += std::chrono::milliseconds(1000 / PS3EYE_FPS);
      std::this_thread::sleep_until(nextTick);
      FillSyntheticFrame(frameBuffer.data(), syntheticIndex++);
    } else {
      camera->getFrame(frameBuffer.data());
    }
    // In stall-simulation mode the loop keeps running but no longer reports
    // frames, so the watchdog sees the same silence a dead camera produces.
    if (!(synthetic == 2 && syntheticIndex > 30))
      noteFrame();
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
  g_captureActive.store(false);
  if (watchdog.joinable())
    watchdog.join();
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

  // Recovery actions: the stall watchdog ends the process on purpose, and this is
  // what brings the service back. Capped at three restarts per day so a camera
  // that is really gone (cable out, dead port) stops churning instead of
  // restarting forever - the SCM gives up and the service stays stopped.
  SC_ACTION actions[3];
  for (int i = 0; i < 3; ++i) {
    actions[i].Type = SC_ACTION_RESTART;
    actions[i].Delay = 5000;
  }
  SERVICE_FAILURE_ACTIONSW failureActions = {};
  failureActions.dwResetPeriod = 24 * 60 * 60;
  failureActions.cActions = 3;
  failureActions.lpsaActions = actions;
  ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions);

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
