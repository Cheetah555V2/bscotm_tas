// Savestates for the injected hook (included by hook.cpp, 32-bit only).
//
// Idea: the game gets all its memory from HeapAlloc/HeapFree/HeapReAlloc (static CRT,
// imported from kernel32), which we can intercept through the exe's own import slots.
// We serve those calls from one private arena whose bookkeeping lives inside the arena,
// so "the game's heap" is a single block of memory that can be copied and put back.
//
// A savestate, taken while the game is frozen at a frame marker, is:
//   - the used part of the arena            (every heap block the game owns)
//   - the exe's writable sections           (.data / .bss: globals, game state)
//   - the main thread's stack from the poll call upwards, plus the callee-saved registers
//     and the SEH chain head at the moment the poll was entered
//   - the virtual clock and the marker number
// Loading puts all of that back and re-enters the poll function from its first instruction,
// on the same stack, so the game carries on as if that marker had just been reached.
//
// Not covered: objects owned by D3D / XAudio / Steam (textures, voices...). The game keeps
// pointers to them in the restored memory, so a state is only valid while those objects
// still exist, i.e. within one stage.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <tlhelp32.h>

// ---- arena allocator -------------------------------------------------------------
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
        Base = (uint8_t*)VirtualAlloc(nullptr, sz, MEM_RESERVE, PAGE_NOACCESS);
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

// ---- snapshots -------------------------------------------------------------------
namespace snap {

static const int MAX_SLOTS = 8;

struct Range { uint8_t* p; uint32_t n; };
static Range Sections[8];
static int   NumSections;

// Callee-saved registers and stack pointer as of the entry of the game's poll call. They are
// captured by the assembly stub below (only for calls coming from the poll site).
extern "C" uint32_t SaveCtx[5] __asm__("_bscotm_savectx");        // ebx esi edi ebp esp
extern "C" uint32_t RestoreCtx[5] __asm__("_bscotm_restorectx");
extern "C" uintptr_t PollSite __asm__("_bscotm_pollsite");
uint32_t SaveCtx[5], RestoreCtx[5];
uintptr_t PollSite;

struct Slot {
    uint8_t* mem = nullptr;
    size_t cap = 0;
    uint32_t arena_used = 0, sec_bytes = 0, stack_esp = 0, stack_bytes = 0;
    uint32_t regs[4] = {};
    uint32_t fs0 = 0;
    uint32_t frame_before = 0;
    int64_t virt = 0;
    bool valid = false;
};
static Slot Slots[MAX_SLOTS];

static void FindSections(HMODULE exe) {
    BYTE* base = (BYTE*)exe;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && NumSections < 8; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        if (!memcmp(sec->Name, ".bind", 5)) continue;       // the copy protection's own section
        uint32_t n = sec->Misc.VirtualSize > sec->SizeOfRawData ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        Sections[NumSections++] = {base + sec->VirtualAddress, (n + 4095) & ~4095u};
    }
}

// While the game memory is copied, every other thread of the process is suspended (and the
// arena lock is held, so none of them is stopped half way through an allocation). Handles
// are opened first because that allocates; between Freeze and Thaw only memcpy runs.
static bool (*ParkHook)();                  // stops the game's own threads at an idle point
static void (*UnparkHook)();
static HANDLE Others[512];
static int    NumOthers;

static void Open() {
    NumOthers = 0;
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (sn == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    for (BOOL ok = Thread32First(sn, &te); ok && NumOthers < 512; ok = Thread32Next(sn, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (h) Others[NumOthers++] = h;
    }
    CloseHandle(sn);
}
static void Freeze() {
    EnterCriticalSection(&arena::Cs);
    for (int i = 0; i < NumOthers; i++) SuspendThread(Others[i]);
}
static void Thaw() {
    for (int i = 0; i < NumOthers; i++) { ResumeThread(Others[i]); CloseHandle(Others[i]); }
    NumOthers = 0;
    LeaveCriticalSection(&arena::Cs);
}

static uint32_t StackBase() { return __readfsdword(4); }   // TEB.StackBase

// Copies everything into slot `slot`. Must run on the game thread, inside the poll call.
static bool Save(int slot, int64_t virt, uint32_t frame_before) {
    Slot& sn = Slots[slot];
    uint32_t used = arena::H->top, secs = 0;
    for (int i = 0; i < NumSections; i++) secs += Sections[i].n;
    uint32_t esp = SaveCtx[4], base = StackBase();
    if (esp >= base || base - esp > (8u << 20)) return false;
    uint32_t ssz = base - esp;
    size_t total = (size_t)used + secs + ssz;
    if (sn.cap < total) {
        if (sn.mem) VirtualFree(sn.mem, 0, MEM_RELEASE);
        sn.cap = (total + (16u << 20)) & ~((1u << 20) - 1);
        sn.mem = (uint8_t*)VirtualAlloc(nullptr, sn.cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!sn.mem) { sn.cap = 0; sn.valid = false; return false; }
    }
    uint8_t* d = sn.mem;
    if (ParkHook) ParkHook();
    Open();
    Freeze();
    memcpy(d, arena::Base, used); d += used;
    for (int i = 0; i < NumSections; i++) { memcpy(d, Sections[i].p, Sections[i].n); d += Sections[i].n; }
    memcpy(d, (void*)esp, ssz);
    Thaw();
    if (UnparkHook) UnparkHook();
    sn.arena_used = used; sn.sec_bytes = secs; sn.stack_esp = esp; sn.stack_bytes = ssz;
    for (int i = 0; i < 4; i++) sn.regs[i] = SaveCtx[i];
    sn.fs0 = __readfsdword(0);
    sn.frame_before = frame_before;
    sn.virt = virt;
    sn.valid = true;
    return true;
}

// ---- restore: runs on a private stack because it overwrites the game's stack ----------
static uint8_t AltStack[64 * 1024] __attribute__((aligned(16)));
static Slot* Pending;
static void (*AfterCopy)(const Slot&);      // host code that fixes up hook state before the jump

extern "C" void RestoreJump() __asm__("_bscotm_restorejump");
extern "C" void PollStub() __asm__("_bscotm_pollstub");

static __attribute__((used, noinline)) void DoRestore() {
    Slot& sn = *Pending;
    const uint8_t* s = sn.mem;
    memcpy(arena::Base, s, sn.arena_used); s += sn.arena_used;
    for (int i = 0; i < NumSections; i++) { memcpy(Sections[i].p, s, Sections[i].n); s += Sections[i].n; }
    memcpy((void*)sn.stack_esp, s, sn.stack_bytes);
    for (int i = 0; i < 4; i++) RestoreCtx[i] = sn.regs[i];
    RestoreCtx[4] = sn.stack_esp;
    __asm__ volatile("mov %0, %%fs:0" : : "r"(sn.fs0));
    Thaw();
    if (UnparkHook) UnparkHook();
    if (AfterCopy) AfterCopy(sn);
    RestoreJump();
    __builtin_unreachable();
}

// Switch to the private stack and never come back.
static __attribute__((noreturn)) void Restore(Slot* sn, void (*after)(const Slot&)) {
    Pending = sn;
    AfterCopy = after;
    if (ParkHook) ParkHook();
    Open();
    Freeze();
    void* top = AltStack + sizeof AltStack - 16;
    __asm__ volatile("mov %0, %%esp\n\tcall *%1" : : "r"(top), "r"(&DoRestore) : "memory");
    __builtin_unreachable();
}

}  // namespace snap

// The IAT target for GetAsyncKeyState. For calls made from the game's poll site it records the
// registers and stack pointer, then jumps to the real hook function `_bscotm_gaks`.
// RestoreJump reloads those registers and re-enters the stub, as if the game had just called.
__asm__(
    ".text\n"
    ".globl _bscotm_pollstub\n"
    "_bscotm_pollstub:\n"
    "    mov (%esp), %eax\n"
    "    cmp _bscotm_pollsite, %eax\n"
    "    jne 1f\n"
    "    mov %ebx, _bscotm_savectx+0\n"
    "    mov %esi, _bscotm_savectx+4\n"
    "    mov %edi, _bscotm_savectx+8\n"
    "    mov %ebp, _bscotm_savectx+12\n"
    "    mov %esp, _bscotm_savectx+16\n"
    "1:  jmp _bscotm_gaks\n"
    ".globl _bscotm_restorejump\n"
    "_bscotm_restorejump:\n"
    "    mov _bscotm_restorectx+0, %ebx\n"
    "    mov _bscotm_restorectx+4, %esi\n"
    "    mov _bscotm_restorectx+8, %edi\n"
    "    mov _bscotm_restorectx+12, %ebp\n"
    "    mov _bscotm_restorectx+16, %esp\n"
    "    jmp _bscotm_pollstub\n");
