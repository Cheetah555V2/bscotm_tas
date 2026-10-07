// Experiment: give the game a private heap at a fixed address (see docs/REVERSE_ENGINEERING.md, RNG).
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string.h>
namespace arena {

static const uint32_t MAGIC = 0xB5C07A11u;
static const uint32_t START = 0x20000;          // first block offset; the header lives before it
static const uint32_t SMALL_MAX = 4096, LARGE_MAX_PAGES = 8192;

struct Header {
    uint32_t top;                               // offset of the next never-used byte
    uint32_t committed;                         // offset up to which pages are committed
    uint32_t small_free[SMALL_MAX / 16 + 1];    // free lists by payload size / 16
    uint32_t large_free[LARGE_MAX_PAGES + 1];   // free lists by payload size / 4096
};
struct Block { uint32_t size, magic, req, pad; };   // 16 bytes in front of every payload

static uint8_t*  Base;
static uint32_t  Size;
static Header*   H;
static CRITICAL_SECTION Cs;

static bool Contains(const void* p) { return (const uint8_t*)p > Base && (const uint8_t*)p < Base + Size; }

static bool Init() {
    static const uint32_t sizes[] = {768u << 20, 512u << 20, 256u << 20};
    for (uint32_t sz : sizes) {
        Base = (uint8_t*)VirtualAlloc((void*)0x30000000, sz, MEM_RESERVE, PAGE_NOACCESS);
        if (!Base) Base = (uint8_t*)VirtualAlloc(nullptr, sz, MEM_RESERVE, PAGE_NOACCESS);
        if (Base) { Size = sz; break; }
    }
    if (!Base) return false;
    if (!VirtualAlloc(Base, START, MEM_COMMIT, PAGE_READWRITE)) return false;
    H = (Header*)Base;
    H->top = START;
    H->committed = START;
    InitializeCriticalSection(&Cs);
    return true;
}

static void* Alloc(size_t n, bool zero) {
    if (n > 0x40000000u) return nullptr;
    uint32_t pay, *bin = nullptr;
    if (n <= SMALL_MAX) {
        pay = ((uint32_t)n + 15) & ~15u;
        if (!pay) pay = 16;
        bin = &H->small_free[pay / 16];
    } else {
        pay = ((uint32_t)n + 4095) & ~4095u;
        if (pay / 4096 <= LARGE_MAX_PAGES) bin = &H->large_free[pay / 4096];
    }
    EnterCriticalSection(&Cs);
    Block* b;
    if (bin && *bin) {
        b = (Block*)*bin - 1;
        *bin = *(uint32_t*)(b + 1);
    } else {
        uint32_t off = H->top, need = 16 + pay;
        if (off + need > Size) { LeaveCriticalSection(&Cs); return nullptr; }
        if (off + need > H->committed) {
            uint32_t to = (off + need + (4u << 20) - 1) & ~((4u << 20) - 1);
            if (to > Size) to = Size;
            if (!VirtualAlloc(Base + H->committed, to - H->committed, MEM_COMMIT, PAGE_READWRITE)) {
                LeaveCriticalSection(&Cs);
                return nullptr;
            }
            H->committed = to;
        }
        b = (Block*)(Base + off);
        b->size = pay;
        b->magic = MAGIC;
        H->top = off + need;
    }
    LeaveCriticalSection(&Cs);
    b->req = (uint32_t)n;
    if (zero) memset(b + 1, 0, b->size);
    return b + 1;
}

static void Free(void* p) {
    Block* b = (Block*)p - 1;
    if (b->magic != MAGIC) return;
    uint32_t* bin = b->size <= SMALL_MAX ? &H->small_free[b->size / 16]
                  : b->size / 4096 <= LARGE_MAX_PAGES ? &H->large_free[b->size / 4096] : nullptr;
    if (!bin) return;                           // huge block: never reused
    EnterCriticalSection(&Cs);
    *(uint32_t*)p = *bin;
    *bin = (uint32_t)p;
    LeaveCriticalSection(&Cs);
}

static size_t RequestedSize(const void* p) { return ((const Block*)p - 1)->req; }
static size_t Capacity(const void* p) { return ((const Block*)p - 1)->size; }
static void SetRequested(void* p, size_t n) { ((Block*)p - 1)->req = (uint32_t)n; }

}  // namespace arena

// ---- game heap -> arena (savestates) ----------------------------------------------
static LPVOID (WINAPI *R_HeapAlloc)(HANDLE, DWORD, SIZE_T);
static BOOL   (WINAPI *R_HeapFree)(HANDLE, DWORD, LPVOID);
static LPVOID (WINAPI *R_HeapReAlloc)(HANDLE, DWORD, LPVOID, SIZE_T);
static SIZE_T (WINAPI *R_HeapSize)(HANDLE, DWORD, LPCVOID);

static void (*AllocLog)(size_t);
static LPVOID WINAPI H_HeapAlloc(HANDLE h, DWORD fl, SIZE_T n) {
    if (AllocLog) AllocLog(n);
    void* p = arena::Alloc(n, (fl & HEAP_ZERO_MEMORY) != 0);
    return p ? p : R_HeapAlloc(h, fl, n);
}

static BOOL WINAPI H_HeapFree(HANDLE h, DWORD fl, LPVOID p) {
    if (!p) return TRUE;
    if (arena::Contains(p)) { arena::Free(p); return TRUE; }
    return R_HeapFree(h, fl, p);
}

static LPVOID WINAPI H_HeapReAlloc(HANDLE h, DWORD fl, LPVOID p, SIZE_T n) {
    if (!p || !arena::Contains(p)) return R_HeapReAlloc(h, fl, p, n);
    size_t old = arena::RequestedSize(p), cap = arena::Capacity(p);
    bool zero = (fl & HEAP_ZERO_MEMORY) != 0;
    if (n <= cap) {
        arena::SetRequested(p, n);
        if (zero && n > old) memset((char*)p + old, 0, n - old);
        return p;
    }
    if (fl & HEAP_REALLOC_IN_PLACE_ONLY) return nullptr;
    void* q = arena::Alloc(n, false);
    if (!q) q = R_HeapAlloc(h, fl & ~HEAP_ZERO_MEMORY, n);
    if (!q) return nullptr;
    memcpy(q, p, old < n ? old : n);
    if (zero && n > old) memset((char*)q + old, 0, n - old);
    arena::Free(p);
    return q;
}

static SIZE_T WINAPI H_HeapSize(HANDLE h, DWORD fl, LPCVOID p) {
    return p && arena::Contains(p) ? arena::RequestedSize(p) : R_HeapSize(h, fl, p);
}

