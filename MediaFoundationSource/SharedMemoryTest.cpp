// SharedMemoryTest.cpp - Quick test to verify shared memory works
#include <iostream>
#include <string>
#include <windows.h>


// Shared memory constants (must match PS3EyeSharedMemory.h)
#define PS3EYE_FRAME_SIZE (640 * 480 * 3)
#define SHARED_MEM_NAME L"PS3EyeSharedFrame"
#define MUTEX_NAME L"PS3EyeFrameMutex"
#define EVENT_NAME L"PS3EyeNewFrameEvent"

struct SharedFrameHeader {
  UINT64 frameNumber;
  UINT64 timestamp;
  UINT32 width;
  UINT32 height;
  UINT32 stride;
  UINT32 format;
  volatile LONG writerActive;
  volatile LONG readerCount;
};

int main() {
  std::wcout << L"PS3 Eye Shared Memory Test\n";
  std::wcout << L"===========================\n\n";

  // Try to open shared memory
  HANDLE hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, SHARED_MEM_NAME);
  if (!hMapFile) {
    std::wcout << L"ERROR: Cannot open shared memory '" << SHARED_MEM_NAME
               << L"'\n";
    std::wcout << L"Make sure PS3EyeCaptureService.exe is running!\n";
    return 1;
  }
  std::wcout << L"[OK] Shared memory opened\n";

  // Try to open mutex
  HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, MUTEX_NAME);
  if (!hMutex) {
    std::wcout << L"ERROR: Cannot open mutex '" << MUTEX_NAME << L"'\n";
    CloseHandle(hMapFile);
    return 1;
  }
  std::wcout << L"[OK] Mutex opened\n";

  // Try to open event
  HANDLE hEvent = OpenEventW(SYNCHRONIZE, FALSE, EVENT_NAME);
  if (!hEvent) {
    std::wcout << L"ERROR: Cannot open event '" << EVENT_NAME << L"'\n";
    CloseHandle(hMutex);
    CloseHandle(hMapFile);
    return 1;
  }
  std::wcout << L"[OK] Event opened\n";

  // Map view
  size_t totalSize = sizeof(SharedFrameHeader) + PS3EYE_FRAME_SIZE;
  void *pBuf = MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, totalSize);
  if (!pBuf) {
    std::wcout << L"ERROR: Cannot map view of file\n";
    CloseHandle(hEvent);
    CloseHandle(hMutex);
    CloseHandle(hMapFile);
    return 1;
  }
  std::wcout << L"[OK] Memory mapped\n\n";

  SharedFrameHeader *header = (SharedFrameHeader *)pBuf;

  std::wcout << L"Reading frames (press Ctrl+C to stop)...\n\n";

  UINT64 lastFrame = 0;
  int frameCount = 0;

  while (frameCount < 10) {
    // Wait for new frame
    DWORD result = WaitForSingleObject(hEvent, 1000);
    if (result == WAIT_TIMEOUT) {
      std::wcout << L"  Waiting for frame...\n";
      continue;
    }

    // Lock mutex
    WaitForSingleObject(hMutex, 100);

    // Read header
    UINT64 frameNum = header->frameNumber;
    UINT32 width = header->width;
    UINT32 height = header->height;

    ReleaseMutex(hMutex);

    if (frameNum != lastFrame) {
      std::wcout << L"  Frame " << frameNum << L" - " << width << L"x" << height
                 << L"\n";
      lastFrame = frameNum;
      frameCount++;
    }
  }

  std::wcout << L"\n[OK] Successfully read " << frameCount << L" frames!\n";
  std::wcout << L"Shared memory communication WORKING!\n";

  // Cleanup
  UnmapViewOfFile(pBuf);
  CloseHandle(hEvent);
  CloseHandle(hMutex);
  CloseHandle(hMapFile);

  return 0;
}
