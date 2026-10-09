// Savestate divergence test (console). Launches the game frozen, plays to --at, saves, runs --gap
// frames (reference), then loads the slot and runs the same frames again --reps times, comparing the
// per-frame player values (the hook's history sampler) with the reference run.
//
//   sstest movie.bscotm --exe COTM.exe --dll bscotm_hook.dll --baseline <dir> --prelude <file>
//          [--at 2000] [--gap 300] [--reps 3] [--speed 50000]
#include <windows.h>
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
static int RunTo(Session& ss, const Frames& mv, uint32_t row, uint32_t speed) {
    uint32_t cur = ss.Row();
    if (row <= cur) return 1;
    if (row > mv.size()) return 0;
    if (!ss.BeginSteps(mv.data() + cur, row - cur, speed)) return 0;
    int r;
    while (!(r = ss.PollSteps())) Sleep(2);
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
        for (int k = 0; k < HIST_FIELDS; k++) {
            if (!Same(it->second.v[k], e.v[k])) {
                float a, b;
                memcpy(&a, &it->second.v[k], 4);
                memcpy(&b, &e.v[k], 4);
                printf("    first difference: frame %u field %s ref=0x%08X (%g) got=0x%08X (%g)\n", e.frame, FIELD[k],
                       it->second.v[k], a, e.v[k], b);
                identical = false;
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
    uint32_t at = 2000, gap = 300, reps = 3, speed = 50000;
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
    p.savestates = true;
    p.hist = true;
    p.speed_milli = speed;
    p.speed_mask = SPEED_ALL;
    if (at + gap + 1 >= p.movie.size()) { puts("movie too short for --at + --gap"); return 2; }

    printf("launching (savestates on)...\n");
    Session ss;
    RunCallbacks cb;
    RunResult rr = RunJob(p, cb, &ss);
    if (!rr.ok || !ss.Active()) { printf("launch failed: %s\n", rr.error.c_str()); return 1; }
    printf("game frozen at row %u, arena %s\n", ss.Row(), (ss.s->features & FEAT_ARENA_OK) ? "ok" : "NOT ok");

    int code = 0;
    do {
        if (!RunTo(ss, p.movie, at, speed)) { puts("FAIL: running to the save point"); code = 1; break; }
        printf("at row %u marker %u: saving...\n", ss.Row(), ss.s->paused);
        DWORD t0 = GetTickCount();
        if (!ss.SaveState(0)) { puts("FAIL: SaveState"); code = 1; break; }
        printf("  saved in %lu ms\n", GetTickCount() - t0);
        printf("  game threads captured: %u (tracked tids %u, skipped %u)\n", ss.s->snap_diag[0], ss.s->snap_diag[6], ss.s->snap_diag[7]);

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
            if (!RunTo(ss, p.movie, at + gap, speed)) { puts("  FAIL: crashed or hung while running on"); code = 1; break; }
            std::vector<HistEntry> got;
            Grab(ss, h0, got);
            bool same;
            size_t ok = Compare(ref, got, same);
            printf("  ran on: %zu samples, %zu matching frames before %s\n", got.size(), ok, same ? "the end (IDENTICAL)" : "the first difference");
            if (same) good++;
        }
        printf("result: %d / %u identical\n", good, reps);
        if (good != (int)reps) code = 1;
    } while (0);

    ss.Close();
    KillGame();
    return code;
}
