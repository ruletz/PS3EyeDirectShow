// PS3EyeSharedMemory.cpp
// Shared memory implementation for lossless PS3 Eye frame sharing

#include "PS3EyeSharedMemory.h"
#include <memoryapi.h>

//------------------------------------------------------------------------------
// PS3EyeSharedMemoryServer Implementation
//------------------------------------------------------------------------------

PS3EyeSharedMemoryServer::PS3EyeSharedMemoryServer()
    : m_fileMapping(nullptr), m_mutex(nullptr), m_newFrameEvent(nullptr),
      m_clientEvent(nullptr), m_sharedMemory(nullptr), m_frameNumber(0) {}

PS3EyeSharedMemoryServer::~PS3EyeSharedMemoryServer() { Close(); }

bool PS3EyeSharedMemoryServer::Create() {
  // Create mutex for synchronization
  m_mutex = CreateMutexW(nullptr, FALSE, PS3EYE_MUTEX_NAME);
  if (!m_mutex) {
    return false;
  }

  // Create event for signaling new frames (auto-reset)
  m_newFrameEvent = CreateEventW(nullptr, FALSE, FALSE, PS3EYE_EVENT_NAME);
  if (!m_newFrameEvent) {
    CloseHandle(m_mutex);
    m_mutex = nullptr;
    return false;
  }

  // Create event for client connect/disconnect notifications (auto-reset)
  m_clientEvent = CreateEventW(nullptr, FALSE, FALSE, PS3EYE_CLIENT_EVENT_NAME);
  if (!m_clientEvent) {
    CloseHandle(m_newFrameEvent);
    CloseHandle(m_mutex);
    m_newFrameEvent = nullptr;
    m_mutex = nullptr;
    return false;
  }

  // Create file mapping for shared memory
  m_fileMapping =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                         PS3EYE_SHARED_MEMORY_SIZE, PS3EYE_SHARED_MEMORY_NAME);

  if (!m_fileMapping) {
    CloseHandle(m_clientEvent);
    CloseHandle(m_newFrameEvent);
    CloseHandle(m_mutex);
    m_clientEvent = nullptr;
    m_newFrameEvent = nullptr;
    m_mutex = nullptr;
    return false;
  }

  // Map view of file
  m_sharedMemory = MapViewOfFile(m_fileMapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                 PS3EYE_SHARED_MEMORY_SIZE);

  if (!m_sharedMemory) {
    CloseHandle(m_fileMapping);
    CloseHandle(m_clientEvent);
    CloseHandle(m_newFrameEvent);
    CloseHandle(m_mutex);
    m_fileMapping = nullptr;
    m_clientEvent = nullptr;
    m_newFrameEvent = nullptr;
    m_mutex = nullptr;
    return false;
  }

  // Initialize header
  PS3EyeFrameHeader *header = static_cast<PS3EyeFrameHeader *>(m_sharedMemory);
  ZeroMemory(header, sizeof(PS3EyeFrameHeader));
  header->magic = PS3EYE_MAGIC;
  header->version = PS3EYE_PROTOCOL_VERSION;
  header->width = PS3EYE_WIDTH;
  header->height = PS3EYE_HEIGHT;
  header->stride = PS3EYE_WIDTH * PS3EYE_BYTES_PER_PIXEL;
  header->format = 0; // RGB24
  header->frameNumber = 0;
  header->timestamp = 0;
  header->dataOffset = sizeof(PS3EyeFrameHeader);
  header->dataSize = PS3EYE_FRAME_SIZE;
  header->serverPID = GetCurrentProcessId();
  header->clientCount = 0;

  m_frameNumber = 0;
  return true;
}

void PS3EyeSharedMemoryServer::Close() {
  if (m_sharedMemory) {
    // Mark as closed
    PS3EyeFrameHeader *header =
        static_cast<PS3EyeFrameHeader *>(m_sharedMemory);
    header->serverPID = 0;

    UnmapViewOfFile(m_sharedMemory);
    m_sharedMemory = nullptr;
  }

  if (m_fileMapping) {
    CloseHandle(m_fileMapping);
    m_fileMapping = nullptr;
  }

  if (m_newFrameEvent) {
    CloseHandle(m_newFrameEvent);
    m_newFrameEvent = nullptr;
  }

  if (m_clientEvent) {
    CloseHandle(m_clientEvent);
    m_clientEvent = nullptr;
  }

  if (m_mutex) {
    CloseHandle(m_mutex);
    m_mutex = nullptr;
  }
}

LONG PS3EyeSharedMemoryServer::GetClientCount() const {
  if (!m_sharedMemory)
    return 0;
  const PS3EyeFrameHeader *header =
      static_cast<const PS3EyeFrameHeader *>(m_sharedMemory);
  return header->clientCount;
}

bool PS3EyeSharedMemoryServer::WaitForClients(DWORD timeoutMs) {
  if (!m_clientEvent)
    return false;

  // Check if already have clients
  if (GetClientCount() > 0)
    return true;

  // Wait for client event
  DWORD result = WaitForSingleObject(m_clientEvent, timeoutMs);
  return (result == WAIT_OBJECT_0 || GetClientCount() > 0);
}

bool PS3EyeSharedMemoryServer::WriteFrame(const uint8_t *frameData,
                                          UINT32 frameSize, UINT64 timestamp) {
  if (!m_sharedMemory || !frameData) {
    return false;
  }

  if (frameSize > PS3EYE_FRAME_SIZE) {
    return false;
  }

  // Acquire mutex
  DWORD waitResult = WaitForSingleObject(m_mutex, 100);
  if (waitResult != WAIT_OBJECT_0) {
    return false;
  }

  // Write frame data
  PS3EyeFrameHeader *header = static_cast<PS3EyeFrameHeader *>(m_sharedMemory);
  uint8_t *frameBuffer =
      static_cast<uint8_t *>(m_sharedMemory) + header->dataOffset;

  // Copy frame (this is the lossless part - just a memory copy)
  memcpy(frameBuffer, frameData, frameSize);

  // Update header
  m_frameNumber++;
  header->frameNumber = m_frameNumber;
  header->timestamp = timestamp;
  header->dataSize = frameSize;

  // Signal new frame available
  SetEvent(m_newFrameEvent);

  // Reset event after brief delay (so clients can catch it)
  // Actually, use manual reset event and reset after clients read

  ReleaseMutex(m_mutex);
  return true;
}

UINT64 PS3EyeSharedMemoryServer::GetFrameNumber() const {
  return m_frameNumber;
}

//------------------------------------------------------------------------------
// PS3EyeSharedMemoryClient Implementation
//------------------------------------------------------------------------------

PS3EyeSharedMemoryClient::PS3EyeSharedMemoryClient()
    : m_fileMapping(nullptr), m_mutex(nullptr), m_newFrameEvent(nullptr),
      m_clientEvent(nullptr), m_sharedMemory(nullptr), m_lastFrameNumber(0) {}

PS3EyeSharedMemoryClient::~PS3EyeSharedMemoryClient() { Disconnect(); }

bool PS3EyeSharedMemoryClient::Connect() {
  // Open existing mutex
  m_mutex = OpenMutexW(SYNCHRONIZE, FALSE, PS3EYE_MUTEX_NAME);
  if (!m_mutex) {
    return false;
  }

  // Open existing event
  m_newFrameEvent = OpenEventW(SYNCHRONIZE, FALSE, PS3EYE_EVENT_NAME);
  if (!m_newFrameEvent) {
    CloseHandle(m_mutex);
    m_mutex = nullptr;
    return false;
  }

  // Open existing file mapping (need write access for clientCount)
  m_fileMapping =
      OpenFileMappingW(FILE_MAP_WRITE, FALSE, PS3EYE_SHARED_MEMORY_NAME);
  if (!m_fileMapping) {
    CloseHandle(m_newFrameEvent);
    CloseHandle(m_mutex);
    m_newFrameEvent = nullptr;
    m_mutex = nullptr;
    return false;
  }

  // Map view of file (write access for clientCount updates)
  m_sharedMemory = MapViewOfFile(m_fileMapping, FILE_MAP_WRITE, 0, 0,
                                 PS3EYE_SHARED_MEMORY_SIZE);

  if (!m_sharedMemory) {
    CloseHandle(m_fileMapping);
    CloseHandle(m_newFrameEvent);
    CloseHandle(m_mutex);
    m_fileMapping = nullptr;
    m_newFrameEvent = nullptr;
    m_mutex = nullptr;
    return false;
  }

  // Validate header
  const PS3EyeFrameHeader *header =
      static_cast<const PS3EyeFrameHeader *>(m_sharedMemory);
  if (header->magic != PS3EYE_MAGIC ||
      header->version != PS3EYE_PROTOCOL_VERSION) {
    Disconnect();
    return false;
  }

  // Open client event to signal server
  m_clientEvent =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, PS3EYE_CLIENT_EVENT_NAME);
  if (m_clientEvent) {
    // Signal that a client has connected
    SetEvent(m_clientEvent);
  }

  // Increment client count (for on-demand mode)
  InterlockedIncrement(&const_cast<PS3EyeFrameHeader *>(header)->clientCount);

  m_lastFrameNumber = 0;
  return true;
}

void PS3EyeSharedMemoryClient::Disconnect() {
  // Decrement client count first (while we still have access)
  if (m_sharedMemory) {
    PS3EyeFrameHeader *header =
        static_cast<PS3EyeFrameHeader *>(m_sharedMemory);
    InterlockedDecrement(&header->clientCount);

    // Signal server that client count changed
    if (m_clientEvent) {
      SetEvent(m_clientEvent);
    }

    UnmapViewOfFile(m_sharedMemory);
    m_sharedMemory = nullptr;
  }

  if (m_fileMapping) {
    CloseHandle(m_fileMapping);
    m_fileMapping = nullptr;
  }

  if (m_newFrameEvent) {
    CloseHandle(m_newFrameEvent);
    m_newFrameEvent = nullptr;
  }

  if (m_clientEvent) {
    CloseHandle(m_clientEvent);
    m_clientEvent = nullptr;
  }

  if (m_mutex) {
    CloseHandle(m_mutex);
    m_mutex = nullptr;
  }
}

bool PS3EyeSharedMemoryClient::WaitForFrame(DWORD timeoutMs) {
  if (!m_newFrameEvent) {
    return false;
  }

  // Wait for new frame signal
  DWORD result = WaitForSingleObject(m_newFrameEvent, timeoutMs);
  return (result == WAIT_OBJECT_0);
}

bool PS3EyeSharedMemoryClient::ReadFrame(uint8_t *destBuffer, UINT32 destSize,
                                         UINT64 *frameNumber,
                                         UINT64 *timestamp) {
  if (!m_sharedMemory || !destBuffer) {
    return false;
  }

  // Acquire mutex for reading
  DWORD waitResult = WaitForSingleObject(m_mutex, 100);
  if (waitResult != WAIT_OBJECT_0) {
    return false;
  }

  const PS3EyeFrameHeader *header =
      static_cast<const PS3EyeFrameHeader *>(m_sharedMemory);

  // Check if server is still running
  if (header->serverPID == 0) {
    ReleaseMutex(m_mutex);
    return false;
  }

  // Check if new frame
  if (header->frameNumber == m_lastFrameNumber) {
    ReleaseMutex(m_mutex);
    return false; // No new frame
  }

  // Copy frame data (lossless - just memory copy)
  UINT32 copySize = min(destSize, header->dataSize);
  const uint8_t *srcBuffer =
      static_cast<const uint8_t *>(m_sharedMemory) + header->dataOffset;
  memcpy(destBuffer, srcBuffer, copySize);

  // Update tracking
  m_lastFrameNumber = header->frameNumber;

  if (frameNumber)
    *frameNumber = header->frameNumber;
  if (timestamp)
    *timestamp = header->timestamp;

  ReleaseMutex(m_mutex);
  return true;
}

bool PS3EyeSharedMemoryClient::GetFrameInfo(UINT32 *width, UINT32 *height,
                                            UINT32 *format,
                                            UINT64 *frameNumber) {
  if (!m_sharedMemory) {
    return false;
  }

  const PS3EyeFrameHeader *header =
      static_cast<const PS3EyeFrameHeader *>(m_sharedMemory);

  if (width)
    *width = header->width;
  if (height)
    *height = header->height;
  if (format)
    *format = header->format;
  if (frameNumber)
    *frameNumber = header->frameNumber;

  return true;
}
