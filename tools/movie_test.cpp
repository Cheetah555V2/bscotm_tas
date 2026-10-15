// Host test for src/movie.cpp (no game needed): malformed input, \u escapes, size limits, and that Save never damages the file on disk.
//   build\movie_test.exe            exit code 0 = all passed
#include "../src/movie.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <string>

static int Failures;
#define CHECK(cond, what) do { if (cond) printf("  ok    %s\n", what); else { printf("  FAIL  %s\n", what); Failures++; } } while (0)

static std::wstring TempFile(const wchar_t* name) {
    wchar_t d[MAX_PATH];
    GetTempPathW(MAX_PATH, d);
    return std::wstring(d) + name;
}
static void WriteText(const std::wstring& p, const std::string& s) {
    FILE* f = _wfopen(p.c_str(), L"wb");
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}
static std::string ReadText(const std::wstring& p) {
    std::string s;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}
// A minimal movie file; `extra` goes in the top-level object, `notes` is the notes array text.
static std::string MovieText(const std::string& frames, const std::string& notes = "[]", const std::string& extra = "") {
    return "{\"format\":\"bscotm-tas\",\"format_version\":1,\"frames\":" + frames + ",\"notes\":" + notes + extra + "}";
}
static bool LoadText(const std::string& text, Movie& m, std::string& err) {
    std::wstring p = TempFile(L"movie_test_case.bscotm");
    WriteText(p, text);
    bool ok = m.Load(p, err);
    DeleteFileW(p.c_str());
    return ok;
}

int main() {
    std::string err;
    Movie m;

    puts("loading");
    CHECK(LoadText(MovieText("[{},{\"87\":1},{},{\"87\":0}]"), m, err) && m.frames.size() == 4, "a small movie loads");
    CHECK(!LoadText("{\"format\":\"bscotm-tas\",\"frames\":[{}]} trailing", m, err), "text after the end is rejected");
    CHECK(!LoadText("not json", m, err), "garbage is rejected");
    CHECK(!LoadText(std::string(100000, '['), m, err), "100 000 nested arrays are rejected without crashing");
    CHECK(!LoadText(MovieText("[{}]", "[{\"frame\":1,\"text\":\"\\u00zz\"}]"), m, err), "a \\u escape with non-hex digits is rejected");
    CHECK(!LoadText(MovieText("[{}]", "[]", ",\"rng_seed\":inf"), m, err), "inf is not a JSON number");
    CHECK(!LoadText(MovieText("[{}]", "[]", ",\"rng_seed\":0x10"), m, err), "hex is not a JSON number");
    CHECK(!LoadText(MovieText("[{}]", "[]", ",\"rng_seed\":+5"), m, err), "a leading + is not a JSON number");

    puts("\\u escapes");
    CHECK(LoadText(MovieText("[{}]", "[{\"frame\":1,\"text\":\"a\\ud83d\\ude00b\"}]"), m, err) && m.notes[0] == "a\xF0\x9F\x98\x80" "b", "a surrogate pair becomes one 4-byte UTF-8 character");
    CHECK(LoadText(MovieText("[{}]", "[{\"frame\":1,\"text\":\"x\\ud83dy\"}]"), m, err) && m.notes[0] == "x\xEF\xBF\xBD" "y", "a lone high surrogate becomes U+FFFD");
    CHECK(LoadText(MovieText("[{}]", "[{\"frame\":1,\"text\":\"\\u00e9\"}]"), m, err) && m.notes[0] == "\xC3\xA9", "a BMP character is 2-byte UTF-8");

    puts("limits and ranges");
    {
        std::string big = "[";
        for (int i = 0; i < MAX_FRAMES; i++) big += i ? ",{}" : "{}";
        big += "]";
        CHECK(!LoadText(MovieText(big), m, err), "a movie longer than the frame limit is rejected");
    }
    CHECK(LoadText(MovieText("[{},{}]", "[{\"frame\":99999999999,\"text\":\"x\"},{\"frame\":0,\"text\":\"y\"}]"), m, err), "notes with out-of-range frames are ignored");
    CHECK(LoadText(MovieText("[{}]", "[]", ",\"clock_version\":1e300"), m, err) && m.clock_version == 0, "an absurd clock_version reads as 0");
    CHECK(LoadText(MovieText("[{}]", "[]", ",\"branches\":[{\"name\":\"b\",\"frame\":4000000000,\"frames\":[{},{}]}]"), m, err) && m.branches.size() == 1 && m.branches[0].frame <= 2,
          "a branch frame past the end is clamped");
    CHECK(LoadText(MovieText("[{\"0\":1}]"), m, err) && m.frames[0] == 0, "the key \"0\" does not turn on a controller key");

    puts("saving");
    std::wstring p = TempFile(L"movie_test_save.bscotm");
    DeleteFileW(p.c_str());
    LoadText(MovieText("[{},{\"87\":1},{}]", "[{\"frame\":2,\"text\":\"note\"}]"), m, err);
    m.rerecords = 7;
    CHECK(m.Save(p, err), "Save writes a new file");
    Movie r;
    CHECK(r.Load(p, err) && r.frames == m.frames && r.notes == m.notes && r.rerecords == 7, "and it loads back the same");
    CHECK(GetFileAttributesW((p + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "no temporary file is left behind");
    std::string before = ReadText(p);
    m.frames.push_back(0);
    CHECK(m.Save(p, err) && ReadText(p) != before, "Save replaces an existing file");
    DeleteFileW(p.c_str());

    // A destination that cannot be replaced (a folder with that name): the save fails, nothing is lost, no temporary file remains.
    CreateDirectoryW(p.c_str(), nullptr);
    CHECK(!m.Save(p, err), "a save that cannot replace the destination reports failure");
    CHECK(GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_DIRECTORY, "and leaves the destination as it was");
    CHECK(GetFileAttributesW((p + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "and removes its temporary file");
    RemoveDirectoryW(p.c_str());

    printf("\n%s\n", Failures ? "FAILED" : "all passed");
    return Failures ? 1 : 0;
}
