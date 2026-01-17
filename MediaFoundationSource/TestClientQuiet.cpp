// TestClientQuiet.cpp - Minimal output client for clean testing
#include <cstdio>
#include <cstdlib>
#include <windows.h>

constexpr wchar_t PS3EYE_SHARED_MEMORY_NAME[] = L"PS3EyeSharedFrame";
constexpr wchar_t PS3EYE_MUTEX_NAME[] = L"PS3EyeFrameMutex";
constexpr wchar_t PS3EYE_CLIENT_EVENT_NAME[] = L"PS3EyeClientEvent";
constexpr UINT32 PS3EYE_MAGIC = 0x45335350;

#pragma pack(push, 1)
struct PS3EyeFrameHeader {
  UINT32 magic, version, width, height, stride, format;
  UINT64 frameNumber, timestamp;
  UINT32 dataOffset, dataSize, serverPID;
  volatile LONG clientCount;
  UINT32 reserved[4];
};
#pragma pack(pop)

constexpr UINT32 PS3EYE_SHARED_MEMORY_SIZE =
    sizeof(PS3EyeFrameHeader) + (640 * 480 * 3);

int main(int argc, char *argv[]) {
  if (argc < 3)
    return 1;

  const char *clientId = argv[1];
  int durationSecs = atoi(argv[2]);

  HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, PS3EYE_MUTEX_NAME);
  if (!mutex)
    return 1;

  HANDLE fileMapping =
      OpenFileMappingW(FILE_MAP_WRITE, FALSE, PS3EYE_SHARED_MEMORY_NAME);
  if (!fileMapping) {
    CloseHandle(mutex);
    return 1;
  }

  void *sharedMem = MapViewOfFile(fileMapping, FILE_MAP_WRITE, 0, 0,
                                  PS3EYE_SHARED_MEMORY_SIZE);
  if (!sharedMem) {
    CloseHandle(fileMapping);
    CloseHandle(mutex);
    return 1;
  }

  PS3EyeFrameHeader *header = (PS3EyeFrameHeader *)sharedMem;
  if (header->magic != PS3EYE_MAGIC) {
    UnmapViewOfFile(sharedMem);
    CloseHandle(fileMapping);
    CloseHandle(mutex);
    return 1;
  }

  HANDLE clientEvent =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, PS3EYE_CLIENT_EVENT_NAME);

  // Connect
  LONG count = InterlockedIncrement(&header->clientCount);
  printf("CLIENT %s CONNECTED (count: %ld)\n", clientId, count);
  if (clientEvent)
    SetEvent(clientEvent);

  // Read frames
  UINT64 lastFrame = 0;
  int framesRead = 0;
  DWORD endTime = GetTickCount() + (durationSecs * 1000);

  while (GetTickCount() < endTime) {
    WaitForSingleObject(mutex, 100);
    if (header->frameNumber != lastFrame) {
      lastFrame = header->frameNumber;
      framesRead++;
    }
    ReleaseMutex(mutex);
    Sleep(33);
  }

  // Disconnect
  count = InterlockedDecrement(&header->clientCount);
  printf("CLIENT %s DISCONNECTED (count: %ld, frames: %d)\n", clientId, count,
         framesRead);
  if (clientEvent)
    SetEvent(clientEvent);

  UnmapViewOfFile(sharedMem);
  CloseHandle(fileMapping);
  if (clientEvent)
    CloseHandle(clientEvent);
  CloseHandle(mutex);

  return 0;
}
