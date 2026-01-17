// PS3EyeSharedMemory.h
// Shared memory interface for PS3 Eye camera frame sharing
// Enables lossless multi-app access on Windows 10+

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <cstdint>
#include <string>
#include <windows.h>

// Frame format constants
constexpr UINT32 PS3EYE_WIDTH = 640;
constexpr UINT32 PS3EYE_HEIGHT = 480;
constexpr UINT32 PS3EYE_FPS = 30;
constexpr UINT32 PS3EYE_BYTES_PER_PIXEL = 3; // RGB24
constexpr UINT32 PS3EYE_FRAME_SIZE =
    PS3EYE_WIDTH * PS3EYE_HEIGHT * PS3EYE_BYTES_PER_PIXEL;

// Shared memory names
constexpr wchar_t PS3EYE_SHARED_MEMORY_NAME[] = L"PS3EyeSharedFrame";
constexpr wchar_t PS3EYE_MUTEX_NAME[] = L"PS3EyeFrameMutex";
constexpr wchar_t PS3EYE_EVENT_NAME[] = L"PS3EyeNewFrameEvent";
constexpr wchar_t PS3EYE_CLIENT_EVENT_NAME[] =
    L"PS3EyeClientEvent"; // Signals server when clients connect/disconnect
constexpr wchar_t PS3EYE_CLIENT_SEMAPHORE_NAME[] =
    L"PS3EyeClientCount"; // Semaphore count = active clients

// Header at the start of shared memory
#pragma pack(push, 1)
struct PS3EyeFrameHeader {
  UINT32 magic;              // 'PS3E' = 0x45335350
  UINT32 version;            // Protocol version (1)
  UINT32 width;              // Frame width
  UINT32 height;             // Frame height
  UINT32 stride;             // Bytes per row
  UINT32 format;             // 0 = RGB24, 1 = BGR24
  UINT64 frameNumber;        // Incrementing frame counter
  UINT64 timestamp;          // Timestamp in 100ns units
  UINT32 dataOffset;         // Offset to frame data from header start
  UINT32 dataSize;           // Size of frame data
  UINT32 serverPID;          // PID of server process
  volatile LONG clientCount; // Number of active clients
  UINT32 reserved[4];        // Future use
};
#pragma pack(pop)

constexpr UINT32 PS3EYE_MAGIC = 0x45335350; // 'PS3E'
constexpr UINT32 PS3EYE_PROTOCOL_VERSION = 1;
constexpr UINT32 PS3EYE_SHARED_MEMORY_SIZE =
    sizeof(PS3EyeFrameHeader) + PS3EYE_FRAME_SIZE;

//------------------------------------------------------------------------------
// PS3EyeSharedMemoryServer
// Used by the capture service to write frames to shared memory
//------------------------------------------------------------------------------
class PS3EyeSharedMemoryServer {
public:
  PS3EyeSharedMemoryServer();
  ~PS3EyeSharedMemoryServer();

  // Initialize shared memory (returns false if already exists)
  bool Create();

  // Close shared memory
  void Close();

  // Write a new frame (thread-safe via mutex)
  bool WriteFrame(const uint8_t *frameData, UINT32 frameSize, UINT64 timestamp);

  // Check if created
  bool IsCreated() const { return m_sharedMemory != nullptr; }

  // Get current frame number
  UINT64 GetFrameNumber() const;

  // Get active client count
  LONG GetClientCount() const;

  // Wait for clients to connect (blocks until at least one client)
  bool WaitForClients(DWORD timeoutMs = INFINITE);

private:
  HANDLE m_fileMapping;
  HANDLE m_mutex;
  HANDLE m_newFrameEvent;
  HANDLE m_clientEvent; // Signaled when clients connect/disconnect
  void *m_sharedMemory;
  UINT64 m_frameNumber;
};

//------------------------------------------------------------------------------
// PS3EyeSharedMemoryClient
// Used by DirectShow filter to read frames from shared memory
//------------------------------------------------------------------------------
class PS3EyeSharedMemoryClient {
public:
  PS3EyeSharedMemoryClient();
  ~PS3EyeSharedMemoryClient();

  // Connect to existing shared memory
  bool Connect();

  // Disconnect
  void Disconnect();

  // Check if connected
  bool IsConnected() const { return m_sharedMemory != nullptr; }

  // Wait for new frame (returns false on timeout or error)
  bool WaitForFrame(DWORD timeoutMs = 100);

  // Read current frame (copies to provided buffer)
  bool ReadFrame(uint8_t *destBuffer, UINT32 destSize,
                 UINT64 *frameNumber = nullptr, UINT64 *timestamp = nullptr);

  // Get frame info without copying
  bool GetFrameInfo(UINT32 *width, UINT32 *height, UINT32 *format,
                    UINT64 *frameNumber);

private:
  HANDLE m_fileMapping;
  HANDLE m_mutex;
  HANDLE m_newFrameEvent;
  HANDLE m_clientEvent; // To signal server when connecting/disconnecting
  void *m_sharedMemory;
  UINT64 m_lastFrameNumber;
};
