// TestClient.cpp - Simple client that connects to shared memory for testing
// Usage: TestClient.exe <client_id> <duration_seconds>

#include <cstdlib>
#include <fstream>
#include <string>
#include <windows.h>


// Must match PS3EyeSharedMemory.h
constexpr wchar_t PS3EYE_SHARED_MEMORY_NAME[] = L"PS3EyeSharedFrame";
constexpr wchar_t PS3EYE_MUTEX_NAME[] = L"PS3EyeFrameMutex";
constexpr wchar_t PS3EYE_CLIENT_EVENT_NAME[] = L"PS3EyeClientEvent";
constexpr UINT32 PS3EYE_MAGIC = 0x45335350;
constexpr UINT32 PS3EYE_PROTOCOL_VERSION = 1;

#pragma pack(push, 1)
struct PS3EyeFrameHeader {
  UINT32 magic;
  UINT32 version;
  UINT32 width;
  UINT32 height;
  UINT32 stride;
  UINT32 format;
  UINT64 frameNumber;
  UINT64 timestamp;
  UINT32 dataOffset;
  UINT32 dataSize;
  UINT32 serverPID;
  volatile LONG clientCount;
  UINT32 reserved[4];
};
#pragma pack(pop)

constexpr UINT32 PS3EYE_SHARED_MEMORY_SIZE =
    sizeof(PS3EyeFrameHeader) + (640 * 480 * 3);

std::ofstream g_log;
std::string g_clientId;

void Log(const char *msg) {
  char buf[512];
  SYSTEMTIME st;
  GetLocalTime(&st);
  sprintf_s(buf, "[%02d:%02d:%02d.%03d] Client %s: %s", st.wHour, st.wMinute,
            st.wSecond, st.wMilliseconds, g_clientId.c_str(), msg);
  g_log << buf << std::endl;
  g_log.flush();
  printf("%s\n", buf);
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    printf("Usage: TestClient.exe <client_id> <duration_seconds>\n");
    return 1;
  }

  g_clientId = argv[1];
  int durationSecs = atoi(argv[2]);

  std::string logFile = "test_client_" + g_clientId + ".txt";
  g_log.open(logFile);

  Log("Starting...");

  // Open mutex
  HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, PS3EYE_MUTEX_NAME);
  if (!mutex) {
    Log("ERROR: Cannot open mutex - is capture service running?");
    return 1;
  }
  Log("Mutex opened");

  // Open shared memory with write access
  HANDLE fileMapping =
      OpenFileMappingW(FILE_MAP_WRITE, FALSE, PS3EYE_SHARED_MEMORY_NAME);
  if (!fileMapping) {
    Log("ERROR: Cannot open shared memory");
    CloseHandle(mutex);
    return 1;
  }
  Log("Shared memory opened");

  // Map view
  void *sharedMem = MapViewOfFile(fileMapping, FILE_MAP_WRITE, 0, 0,
                                  PS3EYE_SHARED_MEMORY_SIZE);
  if (!sharedMem) {
    Log("ERROR: Cannot map shared memory");
    CloseHandle(fileMapping);
    CloseHandle(mutex);
    return 1;
  }

  PS3EyeFrameHeader *header = (PS3EyeFrameHeader *)sharedMem;

  // Validate
  if (header->magic != PS3EYE_MAGIC) {
    Log("ERROR: Invalid magic number");
    UnmapViewOfFile(sharedMem);
    CloseHandle(fileMapping);
    CloseHandle(mutex);
    return 1;
  }
  Log("Header validated");

  // Open client event
  HANDLE clientEvent =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, PS3EYE_CLIENT_EVENT_NAME);

  // INCREMENT client count
  LONG newCount = InterlockedIncrement(&header->clientCount);
  char msg[256];
  sprintf_s(msg, "CONNECTED - clientCount now: %ld", newCount);
  Log(msg);

  // Signal server
  if (clientEvent) {
    SetEvent(clientEvent);
    Log("Server signaled");
  }

  // Wait for specified duration, reading frames
  Log("Reading frames...");
  UINT64 lastFrame = 0;
  int framesRead = 0;
  DWORD startTime = GetTickCount();
  DWORD endTime = startTime + (durationSecs * 1000);

  while (GetTickCount() < endTime) {
    WaitForSingleObject(mutex, 100);
    if (header->frameNumber != lastFrame) {
      lastFrame = header->frameNumber;
      framesRead++;
    }
    ReleaseMutex(mutex);
    Sleep(33); // ~30fps
  }

  sprintf_s(msg, "Read %d frames in %d seconds (%.1f fps)", framesRead,
            durationSecs, (float)framesRead / durationSecs);
  Log(msg);

  // DECREMENT client count
  newCount = InterlockedDecrement(&header->clientCount);
  sprintf_s(msg, "DISCONNECTING - clientCount now: %ld", newCount);
  Log(msg);

  // Signal server
  if (clientEvent) {
    SetEvent(clientEvent);
  }

  // Cleanup
  UnmapViewOfFile(sharedMem);
  CloseHandle(fileMapping);
  if (clientEvent)
    CloseHandle(clientEvent);
  CloseHandle(mutex);

  Log("Disconnected and cleaned up");
  g_log.close();

  return 0;
}
