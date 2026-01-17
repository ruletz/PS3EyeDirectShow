#include <windows.h>
#include <cstdio>
constexpr wchar_t PS3EYE_SHARED_MEMORY_NAME[] = L"PS3EyeSharedFrame";
constexpr wchar_t PS3EYE_MUTEX_NAME[] = L"PS3EyeFrameMutex";
constexpr UINT32 PS3EYE_MAGIC = 0x45335350;
#pragma pack(push, 1)
struct H { UINT32 m,v,w,h,s,f; UINT64 fn,ts; UINT32 o,sz,p; volatile LONG c; UINT32 r[4]; };
#pragma pack(pop)
int main() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, PS3EYE_MUTEX_NAME);
    HANDLE fm = OpenFileMappingW(FILE_MAP_READ, FALSE, PS3EYE_SHARED_MEMORY_NAME);
    H* h = (H*)MapViewOfFile(fm, FILE_MAP_READ, 0, 0, sizeof(H) + 640*480*3);
    if (!h || h->m != PS3EYE_MAGIC) return 1;
    UINT64 last = 0; int cnt = 0;
    DWORD end = GetTickCount() + 5000;
    while (GetTickCount() < end) {
        WaitForSingleObject(m, 50);
        if (h->fn != last) { last = h->fn; cnt++; }
        ReleaseMutex(m);
    }
    printf("Frames in 5s: %d (%.1f fps)\n", cnt, cnt/5.0);
    return 0;
}
