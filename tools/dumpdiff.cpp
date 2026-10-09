// Compares two game-memory dumps made by `sstest --dump ROW --dumpfile F` and lists the values that differ.
// Pointers into the same place of the arena / exe are treated as equal (the arena base differs per launch).
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>

struct Block { uint32_t base = 0; std::vector<uint8_t> mem; std::vector<uint8_t> has; };
struct Dump { uint32_t asz = 0; Block arena, exe; };

static bool ReadBlock(FILE* f, Block& b, uint32_t sizeHint) {
    uint32_t n = 0;
    if (fread(&b.base, 4, 1, f) != 1 || fread(&n, 4, 1, f) != 1) return false;
    b.mem.assign(sizeHint, 0);
    b.has.assign(sizeHint, 0);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off, len;
        if (fread(&off, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1) return false;
        if ((uint64_t)off + len > b.mem.size()) { b.mem.resize(off + len, 0); b.has.resize(off + len, 0); }
        if (fread(&b.mem[off], 1, len, f) != len) return false;
        memset(&b.has[off], 1, len);
    }
    return true;
}
static bool Load(const char* path, Dump& d) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(&d.asz, 4, 1, f) == 1 && ReadBlock(f, d.arena, d.asz) && ReadBlock(f, d.exe, 0x600000);
    fclose(f);
    return ok;
}
static float AsF(uint32_t u) { float x; memcpy(&x, &u, 4); return x; }

int main(int argc, char** argv) {
    if (argc < 3) { puts("usage: dumpdiff a.bin b.bin"); return 2; }
    Dump a, b;
    if (!Load(argv[1], a) || !Load(argv[2], b)) { puts("cannot read the dumps"); return 1; }
    struct Diff { const char* where; uint32_t off, va, vb; };
    std::vector<Diff> diffs;
    size_t ptrdiff = 0, same = 0;
    auto cls = [&](uint32_t v, const Dump& d, uint32_t& off) -> int {      // 1 = pointer into the arena, 2 = pointer into the exe, 0 = a plain value
        if (v - d.arena.base < d.asz) { off = v - d.arena.base; return 1; }
        if (v - d.exe.base < 0x600000) { off = v - d.exe.base; return 2; }
        return 0;
    };
    auto cmp = [&](const char* where, Block& x, Block& y) {
        size_t n = std::min(x.mem.size(), y.mem.size()) & ~3u;
        for (size_t o = 0; o < n; o += 4) {
            if (!x.has[o] || !y.has[o]) continue;
            uint32_t va, vb, oa = 0, ob = 0;
            memcpy(&va, &x.mem[o], 4); memcpy(&vb, &y.mem[o], 4);
            if (va == vb) { same++; continue; }
            int ca = cls(va, a, oa), cb = cls(vb, b, ob);
            if (ca && ca == cb) { if (oa == ob) same++; else ptrdiff++; continue; }
            diffs.push_back({where, (uint32_t)o, va, vb});
        }
    };
    cmp("arena", a.arena, b.arena);
    cmp("exe  ", a.exe, b.exe);
    printf("equal dwords: %zu, differing pointers: %zu, differing values: %zu\n", same, ptrdiff, diffs.size());
    // values that look like timers / counters: small floats or small ints that differ
    int shown = 0;
    for (auto& d : diffs) {
        float fa = AsF(d.va), fb = AsF(d.vb);
        bool floaty = (fa > 1e-3f && fa < 1e6f) && (fb > 1e-3f && fb < 1e6f);
        bool inty = d.va < 100000000u && d.vb < 100000000u;
        if (!floaty && !inty) continue;
        if (strcmp(d.where, "exe  ") != 0) continue;
        if (shown++ >= 150) break;
        printf("  %s +%06X : %08X (%g / %u)  vs  %08X (%g / %u)\n", d.where, d.off, d.va, fa, d.va, d.vb, fb, d.vb);
    }
    return 0;
}
