// Savestate divergence test (console). Launches the game frozen, plays to --at, saves, runs --gap
// frames (reference), then loads the slot and runs the same frames again --reps times, comparing the
// per-frame player values (the hook's history sampler) with the reference run.
//
//   sstest movie.bscotm --exe COTM.exe --dll bscotm_hook.dll --baseline <dir> --prelude <file>
//          [--at 2000] [--gap 300] [--reps 3] [--speed 50000]
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <map>
#include "../src/engine.h"

static const char* FIELD[HIST_FIELDS] = {"health", "wpoints", "score", "xspeed", "yspeed", "x", "y", "invis"};

static void Grab(Session& ss, uint32_t from, std::vector<HistEntry>& out) {
    uint32_t n = ss.s->hist_n;
    out.clear();
    for (uint32_t i = from; i < n; i++) out.push_back(ss.s->hist_buf[i % HIST_MAX]);
}

// Runs from the current row to `row`. 1 = ok, 0 = failed (the game stopped answering).
#include <tlhelp32.h>
// After a failed run: where is every thread of the game stopped? (module + offset of eip, and whether it is waiting)
static void PostMortem(Session& ss) {
    DWORD pid = GetProcessId(ss.proc);
    DWORD ec = 0;
    if (GetExitCodeProcess(ss.proc, &ec) && ec != STILL_ACTIVE) printf("  the game process has exited with code 0x%08lX\n", ec);
    printf("  post-mortem of the game (pid %lu), alive=%d, game marker=%u, frame counter=%u:\n", pid, ss.Alive() ? 1 : 0, ss.s->paused, ss.s->frame);
    std::vector<MODULEENTRY32W> mods;
    HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (ms != INVALID_HANDLE_VALUE) { MODULEENTRY32W me{sizeof me}; for (BOOL ok = Module32FirstW(ms, &me); ok; ok = Module32NextW(ms, &me)) mods.push_back(me); CloseHandle(ms); }
    HANDLE ts = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (ts == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(ts, &te); ok; ok = Thread32Next(ts, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        SuspendThread(h);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(h, &c)) {
            std::wstring where = L"?";
            for (auto& m : mods) if (c.Eip >= (DWORD)(uintptr_t)m.modBaseAddr && c.Eip < (DWORD)(uintptr_t)m.modBaseAddr + m.modBaseSize) {
                wchar_t b[300]; swprintf(b, 300, L"%ls+%X", m.szModule, (unsigned)(c.Eip - (DWORD)(uintptr_t)m.modBaseAddr)); where = b;
            }
            wprintf(L"    tid %5lu eip %08X  %ls\n", te.th32ThreadID, (unsigned)c.Eip, where.c_str());
        }
        CloseHandle(h);        // left suspended on purpose: the game is killed right after
    }
    CloseHandle(ts);
}
// The Memory window's fields, read the same way the editor does (pointer chain from the exe's data).
static void Peek(Session& ss, const char* when) {
    HMODULE mods[256];
    DWORD need = 0;
    uintptr_t base = 0;
    if (EnumProcessModules(ss.proc, mods, sizeof mods, &need) && need) base = (uintptr_t)mods[0];
    struct F { const char* name; uint32_t root; std::vector<uint32_t> x; int kind; };    // kind 0 = u8, 1 = u32, 2 = f32
    const std::vector<uint32_t> pl = {0x84, 0x08, 0x20, 0x20, 0x6C, 0x08};
    auto P = [&](uint32_t f) { std::vector<uint32_t> v{f}; v.insert(v.end(), pl.begin(), pl.end()); return v; };
    std::vector<F> fs = {{"Health", 0x48365C, P(0x3DC), 0}, {"Weapon points", 0x483660, {0x1E, 0x08}, 0}, {"X position", 0x48365C, P(0x1AC), 2},
                         {"Y position", 0x48365C, P(0x1B0), 2}, {"RNG state x", 0x48365C, {0x2F4}, 1}};
    printf("  memory fields %s (exe base %08X):", when, (unsigned)base);
    for (auto& f : fs) {
        uint32_t p = 0;
        SIZE_T g = 0;
        bool ok = ReadProcessMemory(ss.proc, (void*)(base + f.root), &p, 4, &g) && g == 4 && p;
        for (size_t i = f.x.size() - 1; ok && i > 0; i--) ok = ReadProcessMemory(ss.proc, (void*)(p + f.x[i]), &p, 4, &g) && g == 4 && p;
        uint32_t v = 0;
        if (ok) ok = ReadProcessMemory(ss.proc, (void*)(p + f.x[0]), &v, f.kind == 0 ? 1 : 4, &g) && g > 0;
        if (!ok) printf("  %s=-", f.name);
        else if (f.kind == 2) { float fl; memcpy(&fl, &v, 4); printf("  %s=%.2f", f.name, fl); }
        else printf("  %s=%u", f.name, f.kind == 0 ? (v & 0xFF) : v);
    }
    printf("\n");
}

// Dump of the game's writable memory (the arena and the exe's writable sections) for tools/dumpdiff.
static void WriteBlock(FILE* f, HANDLE proc, uintptr_t base, const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
    uint32_t b = (uint32_t)base, n = (uint32_t)ranges.size();
    fwrite(&b, 4, 1, f); fwrite(&n, 4, 1, f);
    std::vector<uint8_t> buf;
    for (auto& r : ranges) {
        fwrite(&r.first, 4, 1, f); fwrite(&r.second, 4, 1, f);
        buf.resize(r.second);
        SIZE_T g = 0;
        if (!ReadProcessMemory(proc, (void*)(base + r.first), buf.data(), r.second, &g)) memset(buf.data(), 0xEE, r.second);
        fwrite(buf.data(), 1, r.second, f);
    }
}
static bool DumpGame(Session& ss, const char* path) {
    // the arena: the biggest reserved private allocation
    uintptr_t abase = 0, asize = 0;
    MEMORY_BASIC_INFORMATION mi;
    for (uintptr_t a = 0x10000; a < 0x7FFE0000;) {
        if (!VirtualQueryEx(ss.proc, (void*)a, &mi, sizeof mi)) break;
        if (mi.Type == MEM_PRIVATE && mi.AllocationBase == mi.BaseAddress) {
            uintptr_t total = 0, q = (uintptr_t)mi.BaseAddress;
            MEMORY_BASIC_INFORMATION m2;
            while (VirtualQueryEx(ss.proc, (void*)q, &m2, sizeof m2) && m2.AllocationBase == mi.AllocationBase) { total += m2.RegionSize; q += m2.RegionSize; }
            if (total >= (200u << 20) && total > asize) { abase = (uintptr_t)mi.AllocationBase; asize = total; }
        }
        a = (uintptr_t)mi.BaseAddress + mi.RegionSize;
    }
    if (!abase) return false;
    std::vector<std::pair<uint32_t, uint32_t>> ar;
    for (uintptr_t q = abase; q < abase + asize;) {
        if (!VirtualQueryEx(ss.proc, (void*)q, &mi, sizeof mi)) break;
        if (mi.State == MEM_COMMIT && (mi.Protect & PAGE_READWRITE)) ar.push_back({(uint32_t)(q - abase), (uint32_t)mi.RegionSize});
        q += mi.RegionSize;
    }
    HMODULE mods[256]; DWORD need = 0;
    EnumProcessModules(ss.proc, mods, sizeof mods, &need);
    uintptr_t ebase = (uintptr_t)mods[0];
    std::vector<uint8_t> hdr(0x1000);
    SIZE_T g = 0;
    ReadProcessMemory(ss.proc, (void*)ebase, hdr.data(), hdr.size(), &g);
    auto nt = (IMAGE_NT_HEADERS*)(hdr.data() + ((IMAGE_DOS_HEADER*)hdr.data())->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    std::vector<std::pair<uint32_t, uint32_t>> er;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE) || !memcmp(sec->Name, ".bind", 5)) continue;
        uint32_t n = sec->Misc.VirtualSize > sec->SizeOfRawData ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        er.push_back({sec->VirtualAddress, (n + 4095) & ~4095u});
    }
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    uint32_t asz = (uint32_t)asize;
    fwrite(&asz, 4, 1, f);
    WriteBlock(f, ss.proc, abase, ar);
    WriteBlock(f, ss.proc, ebase, er);
    fclose(f);
    printf("dumped %zu arena ranges and %zu exe section ranges to %s (arena %08X, exe %08X)\n", ar.size(), er.size(), path, (unsigned)abase, (unsigned)ebase);
    return true;
}


static int RunTo(Session& ss, const Frames& mv, uint32_t row, uint32_t speed) {
    uint32_t cur = ss.Row();
    if (row <= cur) return 1;
    if (row > mv.size()) return 0;
    DWORD t0 = GetTickCount();
    if (!ss.BeginSteps(mv.data() + cur, row - cur, speed)) return 0;
    int r;
    while (!(r = ss.PollSteps())) Sleep(2);
    if (getenv("SSTEST_TIME") && row - cur >= 500) { DWORD ms = GetTickCount() - t0; printf("  [run %u..%u at %ux: %lu ms = %.1f frames/s = %.1fx real time]\n", cur, row, speed / 1000, ms, (row - cur) * 1000.0 / (ms ? ms : 1), (row - cur) * 1000.0 / (ms ? ms : 1) / 60.0); }
    return r > 0;
}

static bool Same(uint32_t a, uint32_t b) { return a == b; }

// Compares two runs of the same frames; prints the first difference. Returns the number of equal frames
// before it (or the total if identical).
static size_t Compare(const std::vector<HistEntry>& ref, const std::vector<HistEntry>& got, bool& identical) {
    std::map<uint32_t, HistEntry> byframe;
    for (const auto& e : ref) byframe[e.frame] = e;
    size_t ok = 0;
    identical = true;
    for (const auto& e : got) {
        auto it = byframe.find(e.frame);
        if (it == byframe.end()) continue;
        if (it->second.v[0] == 0xFFFFFFFFu || e.v[0] == 0xFFFFFFFFu || it->second.v[1] == 0xFFFFFFFFu || e.v[1] == 0xFFFFFFFFu) { ok++; continue; }       // no stage yet (menus, loading): nothing meaningful to compare
        for (int k = 0; k < HIST_FIELDS; k++) {
            auto tinyf = [&](uint32_t u) { float f; memcpy(&f, &u, 4); return k >= 3 && ((f > -1e-20f && f < 1e-20f) || f != f); };     // uninitialised game memory (menus) reads as denormal junk
            if (!Same(it->second.v[k], e.v[k]) && !(tinyf(it->second.v[k]) && tinyf(e.v[k]))) {
                float a, b;
                memcpy(&a, &it->second.v[k], 4);
                memcpy(&b, &e.v[k], 4);
                printf("    first difference: frame %u field %s ref=0x%08X (%g) got=0x%08X (%g)\n", e.frame, FIELD[k],
                       it->second.v[k], a, e.v[k], b);
                {   // context: the frames around the first difference, both runs
                    std::map<uint32_t, HistEntry> gotmap;
                    for (const auto& g2 : got) gotmap[g2.frame] = g2;
                    for (int d = -6; d <= 4; d++) {
                        uint32_t fr = e.frame + d;
                        auto r1 = byframe.find(fr);
                        auto g1 = gotmap.find(fr);
                        if (r1 == byframe.end() || g1 == gotmap.end()) continue;
                        auto fv = [](uint32_t u) { float f; memcpy(&f, &u, 4); return f; };
                        printf("      frame %u%s  ref: hp %u wp %u score %u xs %.3f ys %.3f x %.2f y %.2f | got: hp %u wp %u score %u xs %.3f ys %.3f x %.2f y %.2f\n", fr, d == 0 ? " <--" : "",
                               r1->second.v[0], r1->second.v[1], r1->second.v[2], fv(r1->second.v[3]), fv(r1->second.v[4]), fv(r1->second.v[5]), fv(r1->second.v[6]),
                               g1->second.v[0], g1->second.v[1], g1->second.v[2], fv(g1->second.v[3]), fv(g1->second.v[4]), fv(g1->second.v[5]), fv(g1->second.v[6]));
                    }
                }
                identical = false;
                {   // every stretch of frames that differ (a difference that heals by itself is a reading artifact, e.g. a freed object during a load)
                    uint32_t start = 0, prev = 0, nranges = 0, last = 0;
                    printf("    differing frames:");
                    for (const auto& g2 : got) {
                        auto r1 = byframe.find(g2.frame);
                        if (r1 == byframe.end()) continue;
                        last = g2.frame;
                        bool diff = false;
                        for (int q = 0; q < HIST_FIELDS; q++) if (r1->second.v[q] != g2.v[q]) diff = true;
                        if (diff && !(start && g2.frame == prev + 1)) { if (start) { if (nranges++ < 20) printf(" %u-%u", start, prev); } start = g2.frame; }
                        if (diff) prev = g2.frame;
                    }
                    if (start && nranges++ < 20) printf(" %u-%u", start, prev);
                    printf(" (%u stretches; last compared frame %u)\n", nranges, last);
                }
                return ok;
            }
        }
        ok++;
    }
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { puts("usage: sstest movie.bscotm --exe X --dll X --baseline X --prelude X [--at N] [--gap N] [--reps N] [--speed N]"); return 2; }
    RunParams p;
    std::wstring moviePath = argv[1], preludePath;
    uint32_t at = 2000, gap = 300, reps = 3, speed = 24000, altoff = 0, alts = 3, soak = 0, cmpupto = 0, cmpchunk = 2000, cmpvariant = 0, nostates = 0, peekat = 0, cntfrom = 0, cntto = 0, dropcb = 0, rngscan = 0, maskarg = 0, dumprow = 0, quant = 60, lockarg = 3, lockstep = 0, loadatarg = 0, histarg = 1; std::string dumppath = "dump.bin";
    for (int i = 2; i + 1 < argc; i += 2) {
        std::wstring k = argv[i], v = argv[i + 1];
        if (k == L"--exe") p.exe = v;
        else if (k == L"--dll") p.dll = v;
        else if (k == L"--baseline") p.baseline = v;
        else if (k == L"--prelude") preludePath = v;
        else if (k == L"--at") at = _wtoi(v.c_str());
        else if (k == L"--gap") gap = _wtoi(v.c_str());
        else if (k == L"--reps") reps = _wtoi(v.c_str());
        else if (k == L"--speed") speed = _wtoi(v.c_str());
        else if (k == L"--altoff") altoff = _wtoi(v.c_str());
        else if (k == L"--alts") alts = _wtoi(v.c_str());
        else if (k == L"--soak") soak = _wtoi(v.c_str());
        else if (k == L"--cmp") cmpupto = _wtoi(v.c_str());
        else if (k == L"--cmpchunk") cmpchunk = _wtoi(v.c_str());
        else if (k == L"--variant") cmpvariant = _wtoi(v.c_str());
        else if (k == L"--nostates") nostates = _wtoi(v.c_str());
        else if (k == L"--peek") peekat = _wtoi(v.c_str());
        else if (k == L"--cntfrom") cntfrom = _wtoi(v.c_str());
        else if (k == L"--cntto") cntto = _wtoi(v.c_str());
        else if (k == L"--dropcb") dropcb = _wtoi(v.c_str());
        else if (k == L"--rngscan") rngscan = _wtoi(v.c_str());
        else if (k == L"--mask") maskarg = _wtoi(v.c_str());
        else if (k == L"--quant") quant = _wtoi(v.c_str());
        else if (k == L"--lock") lockarg = _wtoi(v.c_str());
        else if (k == L"--lockstep") lockstep = _wtoi(v.c_str());
        else if (k == L"--loadat") loadatarg = _wtoi(v.c_str());
        else if (k == L"--hist") histarg = _wtoi(v.c_str());
        else if (k == L"--dump") dumprow = _wtoi(v.c_str());
        else if (k == L"--dumpfile") { char b[512]; WideCharToMultiByte(CP_ACP, 0, v.c_str(), -1, b, 512, nullptr, nullptr); dumppath = b; }
    }
    std::string err;
    Movie m;
    if (!m.Load(moviePath, err)) { printf("movie: %s\n", err.c_str()); return 2; }
    if (!preludePath.empty()) {
        Movie pm;
        if (!pm.Load(preludePath, err)) { printf("prelude: %s\n", err.c_str()); return 2; }
        p.prelude = pm.frames;
    } else p.prelude = m.prelude;
    p.movie = m.frames;
    p.seeded = m.has_seed;
    p.seed = m.seed;
    p.backup_dir = L"";
    p.hold = true;
    p.target = 1;
    p.savestates = !nostates;
    p.drop_callbacks = dropcb != 0;
    p.quant_hz = quant;
    p.quant_lock = lockarg;
    p.lockstep = lockstep;
    p.hist = histarg != 0;      // --hist 0: no per-frame sampling (for timing; runs cannot be compared then)
    p.speed_milli = speed;
    p.speed_mask = maskarg ? (uint32_t)maskarg : SPEED_ALL;
    if (!soak && !cmpupto && !peekat && !cntto && !rngscan && !dumprow && at + gap + 1 >= p.movie.size()) { puts("movie too short for --at + --gap"); return 2; }

    if (cmpupto) {      // determinism of the editor's way of running: continuous vs in chunks with states vs with a load in the middle
        struct Run { std::vector<HistEntry> h; bool ok = false; std::vector<uint16_t> t[4]; };
        auto once = [&](const char* name, uint32_t chunk, uint32_t loadAt, uint32_t spd = 0, bool nosave = false, uint32_t launchTo = 0) {
            Run r;
            Session s2;
            RunCallbacks cb2;
            RunParams q = p;
            if (launchTo) q.target = launchTo;      // the editor's Rewind to cursor: the whole schedule is armed at launch
            if (spd) q.speed_milli = spd;
            RunResult rr2 = RunJob(q, cb2, &s2);
            if (!rr2.ok || !s2.Active()) { printf("%s: launch failed: %s\n", name, rr2.error.c_str()); return r; }
            uint32_t from = 0;
            auto grab = [&]() { uint32_t n = s2.s->hist_n; for (uint32_t i = from; i < n; i++) r.h.push_back(s2.s->hist_buf[i % HIST_MAX]); from = n; };
            uint32_t row = s2.Row(), k = 0;
            bool loaded = false;
            while (row < cmpupto) {
                uint32_t end = chunk ? std::min(cmpupto, (row / chunk + 1) * chunk) : cmpupto;
                if (!RunTo(s2, p.movie, end, spd ? spd : speed)) { printf("%s: FAIL running to row %u\n", name, end); PostMortem(s2); s2.Close(); KillGame(); return r; }
                grab();
                row = end;
                if (chunk && row < cmpupto) {
                    if (!nosave && !s2.SaveState(k++ % 8)) { printf("%s: SaveState failed at row %u\n", name, row); s2.Close(); KillGame(); return r; }
                    if (loadAt && !loaded && row >= loadAt) {      // go back one slot, then run on again (the editor's rewind + run to cursor)
                        uint32_t slot = (k - 2) % 8;
                        if (k >= 2 && s2.LoadState(slot)) {
                            uint32_t back = s2.Row();
                            while (!r.h.empty() && r.h.back().frame > back + 1 + (uint32_t)p.prelude.size()) r.h.pop_back();    // those frames will be played again
                            from = s2.s->hist_n;
                            row = back;
                            loaded = true;
                            printf("%s: loaded the state at row %u and ran on\n", name, back);
                        } else { printf("%s: LoadState failed\n", name); s2.Close(); KillGame(); return r; }
                    }
                }
            }
            grab();
            for (int k = 0; k < 4; k++) r.t[k].assign((const uint16_t*)s2.s->thr_log[k], (const uint16_t*)s2.s->thr_log[k] + 12288);
            printf("%s: markers by grid steps since the previous (0,1,2,3,4+): %u %u %u %u %u; irregular after 3000:", name, s2.s->gap_hist[0], s2.s->gap_hist[1], s2.s->gap_hist[2], s2.s->gap_hist[3], s2.s->gap_hist[4]);
            for (uint32_t i = 0; i < s2.s->gap_n && i < 64; i++) printf(" %u:%u", s2.s->gap_log[i] >> 4, s2.s->gap_log[i] & 15);
            printf("\n");
            if (q.quant_lock == 3)
                printf("%s: frame clock: %u ticks/frame (freq %u), music wakes released %u, timed out %u, frames where time ran on by itself %u, markers that waited for real time %u\n",
                       name, s2.s->fc_diag[3], s2.s->fc_diag[4], s2.s->fc_diag[1], s2.s->fc_diag[2], s2.s->fc_diag[0], s2.s->fc_diag[5]);
            if (q.quant_lock == 3 && s2.s->fc_fb_n) {
                printf("%s: %u clock reads ran on by themselves; first ones (marker, caller, marker thread):", name, s2.s->fc_fb_n);
                for (uint32_t i = 0; i < s2.s->fc_fb_n && i < 32; i++) printf(" (%u exe+%X %u)", s2.s->fc_fb_log[i][0], s2.s->fc_fb_log[i][1], s2.s->fc_fb_log[i][2]);
                printf("\n");
            }
            s2.Close();
            KillGame();
            r.ok = true;
            printf("%s: %zu samples\n", name, r.h.size());
            {   // when does health first become 12 after row 10000? (the stage-clear refill)
                uint32_t prev = 0, at12 = 0;
                for (const auto& e : r.h) { if (e.frame > 10000 + p.prelude.size() && prev == 9 && e.v[0] == 12) { at12 = e.frame; break; } prev = e.v[0]; }
                printf("%s: health 9 -> 12 at marker %u\n", name, at12);
            }
            return r;
        };
        Run a = once("continuous", 0, 0);
        Run b = a.ok ? (cmpvariant == 1 ? once("chunked, NO saves", cmpchunk, 0, 0, true) : cmpvariant == 3 ? once("continuous fast, second run", 0, 0) : cmpvariant == 2 ? once("continuous REAL TIME 1x", 0, 0, 1000) : cmpvariant == 4 ? once("editor rewind (armed at launch)", 0, 0, 0, false, cmpupto) : once("chunked+saves", cmpchunk, 0)) : Run();
        Run c = (b.ok && cmpvariant == 0) ? once("chunked+load", cmpchunk, loadatarg ? loadatarg : cmpupto / 2) : Run();
        auto diff = [&](const char* what, const Run& x, const Run& y) {
            bool same;
            size_t ok = Compare(x.h, y.h, same);
            printf("%s: %zu matching frames before %s\n", what, ok, same ? "the end (IDENTICAL)" : "the first difference (above)");
        };
        if (a.ok && b.ok && cmpvariant == 3) {      // thread experiment: how often did each side sleep / wait per marker, in the two runs?
            static const char* TN[4] = {"other threads' Sleep", "other threads' Wait", "marker thread's Sleep", "marker thread's Wait"};
            for (int k = 0; k < 4; k++) {
                uint32_t differing = 0, first = 0;
                for (uint32_t m = 4001; m < 11000 && m < 12288; m++) {
                    int da = (uint16_t)(a.t[k][m] - a.t[k][m - 1]), db = (uint16_t)(b.t[k][m] - b.t[k][m - 1]);
                    if (da != db) { differing++; if (!first) first = m; }
                }
                printf("%s: per-marker count differs in %u of 6999 markers (first at %u);  per marker around 10480..10494:\n  run 1:", TN[k], differing, first);
                for (uint32_t m = 10480; m <= 10494; m++) printf(" %d", (uint16_t)(a.t[k][m] - a.t[k][m - 1]));
                printf("\n  run 2:");
                for (uint32_t m = 10480; m <= 10494; m++) printf(" %d", (uint16_t)(b.t[k][m] - b.t[k][m - 1]));
                printf("\n  totals up to 10492: %u vs %u\n", (uint16_t)(a.t[k][10492] - a.t[k][4000]), (uint16_t)(b.t[k][10492] - b.t[k][4000]));
            }
        }
        if (a.ok && b.ok) diff("continuous vs chunked+saves", a, b);
        if (a.ok && c.ok) diff("continuous vs chunked+load ", a, c);
        return 0;
    }

    printf("launching (savestates on)...\n");
    Session ss;
    RunCallbacks cb;
    RunResult rr = RunJob(p, cb, &ss);
    if (!rr.ok || !ss.Active()) { printf("launch failed: %s\n", rr.error.c_str()); return 1; }
    printf("game frozen at row %u, arena %s\n", ss.Row(), (ss.s->features & FEAT_ARENA_OK) ? "ok" : "NOT ok");

    if (dumprow) {
        if (!RunTo(ss, p.movie, dumprow, speed)) { puts("FAIL"); ss.Close(); KillGame(); return 1; }
        DumpGame(ss, dumppath.c_str());
        ss.Close();
        KillGame();
        return 0;
    }

    if (rngscan) {      // RNG state and health at rows rngscan, +step, ... up to cntto
        for (uint32_t row = rngscan; row <= cntto; row += cntfrom ? cntfrom : 25) {
            if (!RunTo(ss, p.movie, row, speed)) { puts("FAIL"); break; }
            HMODULE mods[256]; DWORD need = 0; uintptr_t base = 0;
            if (EnumProcessModules(ss.proc, mods, sizeof mods, &need) && need) base = (uintptr_t)mods[0];
            auto rd = [&](uintptr_t a, uint32_t& v) { SIZE_T g = 0; return ReadProcessMemory(ss.proc, (void*)a, &v, 4, &g) && g == 4; };
            uint32_t m = 0, rx = 0, ry = 0, rz = 0, rw = 0;
            if (rd(base + 0x48365C, m) && m) { rd(m + 0x2F4, rx); rd(m + 0x2F8, ry); rd(m + 0x2FC, rz); rd(m + 0x300, rw); }
            uint32_t hp = ss.s->hist_n ? ss.s->hist_buf[(ss.s->hist_n - 1) % HIST_MAX].v[0] : 0;
            printf("row %u: rng %08X %08X %08X %08X  hp %u\n", row, rx, ry, rz, rw, hp);
        }
        ss.Close();
        KillGame();
        return 0;
    }

    if (cntto) {        // which XAudio2 methods does the game call, and which callbacks arrive, between two rows?
        static const char* PXN[29] = {"GetVoiceDetails", "SetOutputVoices", "SetEffectChain", "EnableEffect", "DisableEffect", "GetEffectState", "SetEffectParameters",
            "GetEffectParameters", "SetFilterParameters", "GetFilterParameters", "SetOutputFilterParameters", "GetOutputFilterParameters", "SetVolume", "GetVolume",
            "SetChannelVolumes", "GetChannelVolumes", "SetOutputMatrix", "GetOutputMatrix", "DestroyVoice(unused)", "Start", "Stop", "SubmitSourceBuffer",
            "FlushSourceBuffers", "Discontinuity", "ExitLoop", "GetState", "SetFrequencyRatio", "GetFrequencyRatio", "SetSourceSampleRate"};
        static const char* SHN[7] = {"OnVoiceProcessingPassStart", "OnVoiceProcessingPassEnd", "OnStreamEnd", "OnBufferStart", "OnBufferEnd", "OnLoopEnd", "OnVoiceError"};
        if (!RunTo(ss, p.movie, cntfrom, speed)) { puts("FAIL"); ss.Close(); KillGame(); return 1; }
        uint32_t a[32], b[8];
        for (int i = 0; i < 32; i++) a[i] = ss.s->px_cnt[i];
        for (int i = 0; i < 8; i++) b[i] = ss.s->sh_cnt[i];
        if (!RunTo(ss, p.movie, cntto, speed)) { puts("FAIL"); ss.Close(); KillGame(); return 1; }
        printf("rows %u..%u: game calls on voices:", cntfrom, cntto);
        for (int i = 0; i < 29; i++) if (ss.s->px_cnt[i] != a[i]) printf("  %s x%u", PXN[i], ss.s->px_cnt[i] - a[i]);
        printf("\n                      callbacks received:");
        for (int i = 0; i < 7; i++) if (ss.s->sh_cnt[i] != b[i]) printf("  %s x%u", SHN[i], ss.s->sh_cnt[i] - b[i]);
        printf("\n");
        ss.Close();
        KillGame();
        return 0;
    }

    if (peekat) {       // what would the Memory window show?
        if (!RunTo(ss, p.movie, peekat, speed)) { printf("FAIL: running (game marker %u)\n", ss.s->frame); PostMortem(ss); ss.Close(); KillGame(); return 1; }
        Peek(ss, "at the stopping point");
        if (!nostates) {
            if (!ss.SaveState(0)) { puts("FAIL: SaveState"); ss.Close(); KillGame(); return 1; }
            Peek(ss, "after SaveState");
            RunTo(ss, p.movie, peekat + 300, speed);
            Peek(ss, "300 frames later");
            if (!ss.LoadState(0)) { puts("FAIL: LoadState"); ss.Close(); KillGame(); return 1; }
            Peek(ss, "right after LoadState");
            RunTo(ss, p.movie, peekat + 100, speed);
            Peek(ss, "100 frames after the load");
        }
        ss.Close();
        KillGame();
        return 0;
    }

    if (soak) {         // memory use over a long movie: run forward, save a state into a rotating slot every `soak` frames
        auto mem = [&](const char* tag, uint32_t row, DWORD ms) {
            PROCESS_MEMORY_COUNTERS_EX m{};
            GetProcessMemoryInfo(ss.proc, (PROCESS_MEMORY_COUNTERS*)&m, sizeof m);
            printf("%-6s row %6u: private %5.0f MB  working set %5.0f MB  peak working set %5.0f MB  (%lu ms)\n", tag, row, m.PrivateUsage / 1048576.0,
                   m.WorkingSetSize / 1048576.0, m.PeakWorkingSetSize / 1048576.0, ms);
            fflush(stdout);
        };
        mem("start", ss.Row(), 0);
        uint32_t i = 0;
        for (uint32_t row = soak; row + 2 < p.movie.size(); row += soak, i++) {
            DWORD t0 = GetTickCount();
            if (!RunTo(ss, p.movie, row, speed)) { printf("FAIL: running to row %u\n", row); ss.Close(); KillGame(); return 1; }
            DWORD t1 = GetTickCount();
            if (!ss.SaveState(i % 8)) { printf("FAIL: SaveState at row %u\n", row); ss.Close(); KillGame(); return 1; }
            mem("saved", row, GetTickCount() - t1);
            printf("         state pages %u, copied %u, pool in use %u pages (%.0f MB)\n", ss.s->snap_stats[2], ss.s->snap_stats[1], ss.s->snap_stats[0],
                   ss.s->snap_stats[0] * 4096 / 1048576.0);
            (void)t0;
        }
        printf("loading slot 0 and running 300 frames...\n");
        if (!ss.LoadState(0)) puts("FAIL: LoadState");
        else if (!RunTo(ss, p.movie, ss.Row() + 300, speed)) puts("FAIL: crashed or hung after the load");
        else puts("ok: survived the load");
        mem("end", ss.Row(), 0);
        ss.Close();
        KillGame();
        return 0;
    }

    int code = 0;
    do {
        if (!RunTo(ss, p.movie, at, speed)) { puts("FAIL: running to the save point"); code = 1; break; }
        printf("at row %u marker %u: saving...\n", ss.Row(), ss.s->paused);
        DWORD t0 = GetTickCount();
        if (!ss.SaveState(0)) { puts("FAIL: SaveState"); code = 1; break; }
        printf("  saved in %lu ms\n", GetTickCount() - t0);
        printf("  game threads captured: %u (tracked tids %u, skipped %u)\n", ss.s->snap_diag[0], ss.s->snap_diag[6], ss.s->snap_diag[7]);
        printf("  save phases (ms): engine pause %u, park threads %u, compare %u, reserve pool %u, open+freeze %u, copy %u | Save total %u, engine resume %u\n",
               ss.s->snap_time[0], ss.s->snap_time[1], ss.s->snap_time[2], ss.s->snap_time[3], ss.s->snap_time[4], ss.s->snap_time[5], ss.s->snap_time[6], ss.s->snap_time[7]);

        uint32_t h0 = ss.s->hist_n;
        if (!RunTo(ss, p.movie, at + gap, speed)) { puts("FAIL: reference run crashed or hung"); code = 1; break; }
        std::vector<HistEntry> ref;
        Grab(ss, h0, ref);
        printf("reference: %zu samples, markers %u..%u\n", ref.size(), ref.empty() ? 0 : ref.front().frame, ref.empty() ? 0 : ref.back().frame);

        int good = 0;
        for (uint32_t r = 1; r <= reps; r++) {
            printf("rep %u: loading...\n", r);
            t0 = GetTickCount();
            if (!ss.LoadState(0)) { puts("  FAIL: LoadState"); code = 1; break; }
            printf("  loaded in %lu ms, marker %u row %u\n", GetTickCount() - t0, ss.s->paused, ss.Row());
            printf("  threads restored %u, mismatched %u, missing %u (first mismatch eip saved %08X now %08X)\n", ss.s->snap_diag[1],
                   ss.s->snap_diag[2], ss.s->snap_diag[3], ss.s->snap_diag[4], ss.s->snap_diag[5]);
            h0 = ss.s->hist_n;
            if (!RunTo(ss, p.movie, at + gap, speed)) { puts("  FAIL: crashed or hung while running on"); PostMortem(ss); code = 1; break; }
            std::vector<HistEntry> got;
            Grab(ss, h0, got);
            bool same;
            size_t ok = Compare(ref, got, same);
            printf("  ran on: %zu samples, %zu matching frames before %s\n", got.size(), ok, same ? "the end (IDENTICAL)" : "the first difference");
            if (same) good++;
        }
        printf("result: %d / %u identical\n", good, reps);

        if (altoff) {       // the real use: load, then play different inputs. Not comparable, only has to survive.
            for (uint32_t r = 1; r <= alts; r++) {
                if (!ss.LoadState(0)) { puts("  FAIL: LoadState before the different-input run"); code = 1; break; }
                uint32_t from = at + altoff * r;
                if (from + gap >= p.movie.size()) from = (uint32_t)p.movie.size() - gap - 1;
                if (!ss.BeginSteps(p.movie.data() + from, gap, speed)) { puts("  FAIL: BeginSteps"); code = 1; break; }
                int pr;
                while (!(pr = ss.PollSteps())) Sleep(2);
                if (pr < 0) { printf("  FAIL: crashed or hung with different inputs (taken from row %u)\n", from); code = 1; break; }
                printf("  different inputs (from row %u): survived %u frames\n", from, gap);
            }
        }
        if (good != (int)reps) code = 1;
    } while (0);

    ss.Close();
    KillGame();
    return code;
}
