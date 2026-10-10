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

// ---- the game's own threads: registers and stacks ---------------------------------------
// The game's long-lived threads (pacer, logic, ...) keep their own locals on their own stacks. Put back
// only the heap and the main thread, and such a thread wakes up holding pointers to objects of another
// moment. So at a save every tracked game thread (stopped inside a Sleep/Wait) has its registers and its
// stack recorded; at a load the same is put back, but only for a thread stopped at the very same place
// (same eip and esp), so a changed situation is reported instead of silently corrupted.
static DWORD (*GameTids)(DWORD* out, DWORD max);     // the hook's tracked game threads
struct ThreadInfo { DWORD tid; uint32_t eip, esp, start; bool tracked; };
static ThreadInfo Info[64];
static int NInfo;
static volatile uint32_t* Diag;                       // -> Shm::snap_diag
static const int MAX_THREADS = 16;
static const size_t THREAD_MEM = 4u << 20;
struct ThreadRec { DWORD tid; uint32_t esp, base, off; CONTEXT ctx; };

// ---- page pool: saved states share the pages that did not change ---------------------------------
// A state is a table of 4 KB page indices (arena pages, then the exe's writable sections). When a page is
// byte-identical to the same page of the previous state it is not copied again: both tables point at it
// (reference counted). A game that changes little between two saves costs little per state, which matters
// in a 32-bit process (2 GB address space).
namespace pagepool {
static const uint32_t PG = 4096, CHUNK_PAGES = 1024, MAX_CHUNKS = 160;     // chunks of 4 MB, at most 640 MB
static uint8_t*  Chunk[MAX_CHUNKS];
static uint32_t  NChunks;
static uint16_t  Ref[MAX_CHUNKS * CHUNK_PAGES];
static uint32_t  FreeList[MAX_CHUNKS * CHUNK_PAGES];
static uint32_t  NFree, InUse;
static inline uint8_t* Ptr(uint32_t i) { return Chunk[i >> 10] + (i & 1023) * PG; }
static bool Grow() {
    if (NChunks >= MAX_CHUNKS) return false;
    uint8_t* c = (uint8_t*)VirtualAlloc(nullptr, CHUNK_PAGES * PG, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!c) return false;
    Chunk[NChunks] = c;
    for (uint32_t i = CHUNK_PAGES; i-- > 0;) FreeList[NFree++] = (NChunks << 10) + i;
    NChunks++;
    return true;
}
static bool Reserve(uint32_t n) { while (NFree < n) if (!Grow()) return false; return true; }
static inline uint32_t Alloc() { uint32_t i = FreeList[--NFree]; Ref[i] = 1; InUse++; return i; }
static inline void Retain(uint32_t i) { Ref[i]++; }
static inline void Release(uint32_t i) { if (--Ref[i] == 0) { FreeList[NFree++] = i; InUse--; } }
}  // namespace pagepool

struct Slot {
    uint8_t* mem = nullptr;                 // the main thread's stack
    size_t cap = 0;
    uint32_t* pg = nullptr;                 // page table: arena pages, then section pages (indices into the page pool)
    uint32_t npg = 0, tabcap = 0;
    uint32_t arena_used = 0, sec_bytes = 0, stack_esp = 0, stack_bytes = 0;
    uint32_t regs[4] = {};
    uint32_t fs0 = 0;
    uint32_t frame_before = 0;
    int64_t virt = 0;
    int64_t aux[2] = {};                    // the hook's frame-locked clock state at the save (LockBase, LockServed)
    ThreadRec th[MAX_THREADS];
    int nth = 0;
    uint8_t* tmem = nullptr;
    bool valid = false;
};
static Slot Slots[MAX_SLOTS];
static int LastSlot = -1;                   // the most recently saved or loaded slot: the next save is compared with it
static int64_t Aux[2];                       // set by the hook just before Save; stored in the slot
static volatile uint32_t* Stats;             // -> Shm::snap_stats
static volatile uint32_t* Times;             // -> Shm::snap_time

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
static HANDLE   Others[512];
static DWORD    OthersTid[512];
static uint32_t OthersBase[512], OthersLimit[512];    // stack bounds (TEB.StackBase / StackLimit)
static uint32_t OthersStart[512];                      // Win32 start address of each thread
static int      NumOthers;

typedef LONG (NTAPI *PFN_NtQueryInformationThread)(HANDLE, ULONG, PVOID, ULONG, PULONG);

static void Open() {
    NumOthers = 0;
    static PFN_NtQueryInformationThread NtQit = (PFN_NtQueryInformationThread)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (sn == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    for (BOOL ok = Thread32First(sn, &te); ok && NumOthers < 512; ok = Thread32Next(sn, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!h) continue;
        uint32_t base = 0, limit = 0;
        if (NtQit) {
            struct { LONG exit; PVOID teb; DWORD pid, tid; ULONG_PTR aff; LONG prio, baseprio; } tbi = {};
            if (NtQit(h, 0, &tbi, sizeof tbi, nullptr) == 0 && tbi.teb) {
                base = *(uint32_t*)((char*)tbi.teb + 4);
                limit = *(uint32_t*)((char*)tbi.teb + 8);
            }
        }
        uint32_t start = 0;
        if (NtQit) { uint32_t a = 0; if (NtQit(h, 9, &a, 4, nullptr) == 0) start = a; }     // ThreadQuerySetWin32StartAddress
        OthersStart[NumOthers] = start;
        Others[NumOthers] = h; OthersTid[NumOthers] = te.th32ThreadID; OthersBase[NumOthers] = base; OthersLimit[NumOthers] = limit;
        NumOthers++;
    }
    CloseHandle(sn);
}
static int OtherIndex(DWORD tid) { for (int i = 0; i < NumOthers; i++) if (OthersTid[i] == tid) return i; return -1; }

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
    using namespace pagepool;
    uint32_t anp = (used + PG - 1) / PG, npg = anp + secs / PG;
    if (sn.cap < ssz) {                          // the main stack store (small)
        if (sn.mem) VirtualFree(sn.mem, 0, MEM_RELEASE);
        sn.cap = (ssz + (1u << 20)) & ~((1u << 20) - 1);
        sn.mem = (uint8_t*)VirtualAlloc(nullptr, sn.cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!sn.mem) { sn.cap = 0; sn.valid = false; return false; }
    }
    // everything that allocates is done before the threads are stopped
    if (!sn.tmem) sn.tmem = (uint8_t*)VirtualAlloc(nullptr, THREAD_MEM, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    size_t tabbytes = (((size_t)npg * 4 + npg) + 4095) & ~(size_t)4095;
    uint32_t* newtab = (uint32_t*)VirtualAlloc(nullptr, tabbytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!newtab) return false;
    uint8_t* chg = (uint8_t*)(newtab + npg);     // chg[i] = page i differs from the previous state
    auto srcpage = [&](uint32_t i, uint32_t& len) -> const uint8_t* {
        if (i < anp) { len = (i + 1 == anp) ? used - i * PG : PG; return arena::Base + i * PG; }
        uint32_t off = (i - anp) * PG;
        for (int s = 0; s < NumSections; s++) { if (off < Sections[s].n) { len = PG; return Sections[s].p + off; } off -= Sections[s].n; }
        len = 0;
        return nullptr;
    };
    const Slot* prev = (LastSlot >= 0 && Slots[LastSlot].valid) ? &Slots[LastSlot] : nullptr;
    DWORD tm0 = GetTickCount();
    if (ParkHook && !ParkHook()) {              // a game thread is in the middle of a job: do not save a half-finished moment
        if (UnparkHook) UnparkHook();
        VirtualFree(newtab, 0, MEM_RELEASE);
        return false;
    }
    DWORD tm1 = GetTickCount();
    if (tm1 - tm0 > 20) {                          // stopping the threads took a while: the game is busy (loading), its jobs are half done
        if (Times) Times[1] = tm1 - tm0;
        if (Diag) Diag[0] = 0xFFFFFFFFu;
        if (UnparkHook) UnparkHook();
        VirtualFree(newtab, 0, MEM_RELEASE);
        return false;
    }
    uint32_t need = 0;
    for (uint32_t i = 0; i < npg; i++) {
        uint32_t len;
        const uint8_t* p = srcpage(i, len);
        chg[i] = !(prev && i < prev->npg && !memcmp(p, Ptr(prev->pg[i]), len));
        need += chg[i];
    }
    DWORD tm2 = GetTickCount();
    if (!Reserve(need + 16)) { if (UnparkHook) UnparkHook(); VirtualFree(newtab, 0, MEM_RELEASE); return false; }
    DWORD tm3 = GetTickCount();
    Open();
    Freeze();
    DWORD tm4 = GetTickCount();
    uint32_t built = 0;
    for (; built < npg; built++) {
        uint32_t len;
        const uint8_t* p = srcpage(built, len);
        if (!chg[built]) { newtab[built] = prev->pg[built]; Retain(newtab[built]); continue; }
        if (!NFree) break;
        uint32_t idx = Alloc();
        memcpy(Ptr(idx), p, len);
        newtab[built] = idx;
    }
    if (built < npg) {                           // the pool ran dry: undo
        for (uint32_t j = 0; j < built; j++) Release(newtab[j]);
        Thaw();
        if (UnparkHook) UnparkHook();
        VirtualFree(newtab, 0, MEM_RELEASE);
        return false;
    }
    DWORD tm5 = GetTickCount();
    memcpy(sn.mem, (void*)esp, ssz);
    uint32_t* oldtab = sn.pg;
    uint32_t oldn = sn.npg;
    if (oldtab) for (uint32_t j = 0; j < oldn; j++) Release(oldtab[j]);
    sn.pg = newtab;
    sn.npg = npg;
    if (Stats) { Stats[0] = InUse; Stats[1] = need; Stats[2] = npg; Stats[3] = NFree; }
    if (Times) { Times[1] = tm1 - tm0; Times[2] = tm2 - tm1; Times[3] = tm3 - tm2; Times[4] = tm4 - tm3; Times[5] = tm5 - tm4; }
    sn.nth = 0;
    if (GameTids && sn.tmem) {
        DWORD tids[MAX_THREADS];
        DWORD n = GameTids(tids, MAX_THREADS);
        uint32_t skipped = 0;
        if (Diag) Diag[6] = n;
        uint32_t off = 0;
        for (DWORD t = 0; t < n && sn.nth < MAX_THREADS; t++) {
            int k = OtherIndex(tids[t]);
            if (k < 0 || !OthersBase[k]) { skipped++; continue; }
            ThreadRec& r = sn.th[sn.nth];
            r.ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (!GetThreadContext(Others[k], &r.ctx)) { skipped++; continue; }
            uint32_t tesp = r.ctx.Esp, tbase = OthersBase[k];
            if (tesp < OthersLimit[k] || tesp >= tbase || tbase - tesp > THREAD_MEM - off) { skipped++; continue; }
            memcpy(sn.tmem + off, (void*)tesp, tbase - tesp);
            r.tid = tids[t]; r.esp = tesp; r.base = tbase; r.off = off;
            off += (tbase - tesp + 15) & ~15u;
            sn.nth++;
        }
    NInfo = 0;                                  // every other thread of the process: where is it stopped, and does the hook track it?
    for (int i = 0; i < NumOthers && NInfo < 64; i++) {
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        bool got = GetThreadContext(Others[i], &c) != 0;
        bool tracked = false;
        if (GameTids) { DWORD tt[MAX_THREADS]; DWORD nn = GameTids(tt, MAX_THREADS); for (DWORD q = 0; q < nn; q++) if (tt[q] == OthersTid[i]) tracked = true; }
        Info[NInfo++] = { OthersTid[i], got ? c.Eip : 0, got ? c.Esp : 0, OthersStart[i], tracked };
    }
    if (Diag) Diag[7] = skipped;
    }
    if (Diag) { Diag[0] = (uint32_t)sn.nth; }
    Thaw();
    if (UnparkHook) UnparkHook();
    if (oldtab) VirtualFree(oldtab, 0, MEM_RELEASE);
    sn.arena_used = used; sn.sec_bytes = secs; sn.stack_esp = esp; sn.stack_bytes = ssz;
    for (int i = 0; i < 4; i++) sn.regs[i] = SaveCtx[i];
    sn.fs0 = __readfsdword(0);
    sn.frame_before = frame_before;
    sn.virt = virt;
    sn.aux[0] = Aux[0]; sn.aux[1] = Aux[1];
    sn.valid = true;
    LastSlot = slot;
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
    {
        using namespace pagepool;
        uint32_t anp = (sn.arena_used + PG - 1) / PG;
        for (uint32_t i = 0; i < sn.npg; i++) {
            uint8_t* dst;
            uint32_t len;
            if (i < anp) { dst = arena::Base + i * PG; len = (i + 1 == anp) ? sn.arena_used - i * PG : PG; }
            else {
                uint32_t off = (i - anp) * PG;
                dst = nullptr; len = 0;
                for (int k = 0; k < NumSections; k++) { if (off < Sections[k].n) { dst = Sections[k].p + off; len = PG; break; } off -= Sections[k].n; }
            }
            if (dst) memcpy(dst, Ptr(sn.pg[i]), len);
        }
        LastSlot = (int)(&sn - Slots);          // the game now equals this state: the next save is compared with it
    }
    memcpy((void*)sn.stack_esp, sn.mem, sn.stack_bytes);
    if (Diag) { Diag[1] = Diag[2] = Diag[3] = Diag[4] = Diag[5] = 0; }
    for (int i = 0; i < sn.nth; i++) {
        const ThreadRec& r = sn.th[i];
        int k = OtherIndex(r.tid);
        if (k < 0) { if (Diag) Diag[3]++; continue; }
        CONTEXT cur;
        cur.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (!GetThreadContext(Others[k], &cur) || cur.Esp != r.ctx.Esp || cur.Eip != r.ctx.Eip) {
            if (Diag) { if (!Diag[2]) { Diag[4] = r.ctx.Eip; Diag[5] = GetThreadContext(Others[k], &cur) ? cur.Eip : 0; } Diag[2]++; }
            continue;
        }
        memcpy((void*)r.esp, sn.tmem + r.off, r.base - r.esp);
        CONTEXT set = r.ctx;
        set.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        SetThreadContext(Others[k], &set);
        if (Diag) Diag[1]++;
    }
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
