// .gdfwr container: binary, little-endian, sections with CRC32 (Part 2, 2.2) and atomic save (Part 8, 8.4).
#pragma once
#include <ctime>
#include <filesystem>

#include "Model.hpp"

namespace gdfwr {

// Section versions written by this mod. MACR v2 adds the sub-tick fraction of every event; ANAL v2 adds the search limit, the
// per-action mode, the edges and widths of every window. Older versions are still read; they can be written for tests.
inline constexpr uint16_t kMacroSectionVersion = 2, kAnalysisSectionVersion = 2;
std::vector<uint8_t> serialize(const GdfwrFile& f, uint16_t macroVersion = kMacroSectionVersion,
                               uint16_t analysisVersion = kAnalysisSectionVersion);

struct LoadResult {
    bool ok = false;
    std::string errorCode;               // LOAD_NEEDS_NEWER_MOD | LOAD_FILE_DAMAGED
    std::string errorText;
    GdfwrFile file;
    std::vector<Warning> loadWarnings;   // DERIVED_DROPPED ... (not stored in the file)
};
// Part 8, 8.5.1 steps 3-5: header, CRC of every section, unknown sections skipped, stale rule (2.9).
LoadResult parse(const uint8_t* data, size_t n);

// Part 8, 8.5.1 step 6.
enum class LevelMatch { NoLevelLoaded, Same, Changed, Mismatch };
LevelMatch compareLevel(const LevelInfo& inFile, bool haveLoadedLevel, int64_t loadedId, uint64_t loadedHash);
// LEVEL_CHANGED: BASELINE and ANALYSIS become STALE. LEVEL_MISMATCH: file opens, Replay/Verify/Analysis are off.
void applyLevelMatch(GdfwrFile& f, LevelMatch m, std::vector<Warning>& out);

struct SaveResult {
    bool ok = false;
    std::string errorCode, errorText;
};
// tmp file in the same folder -> flush -> .bak of the previous version -> rename over the old file.
SaveResult saveAtomic(const std::filesystem::path& path, const GdfwrFile& f, bool keepBackup = true);
bool readWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out);
bool writeAtomicBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes, bool keepBackup,
                      std::string* err);

std::string sanitizeFileName(std::string s);
std::string defaultFileName(const std::string& levelName, const std::tm& t, bool withTime, const std::string& ext);

// Analysis gate (Part 3 / Part 5): empty string = allowed, otherwise the reason.
std::string analysisGate(const GdfwrFile& f, bool levelMatches);
bool hasWarning(const GdfwrFile& f, std::string_view code);
// "ANALYSIS: CURRENT | STALE | NONE" of the File panel status line (Part 8, 8.2).
const char* analysisStateName(const GdfwrFile& f);

}  // namespace gdfwr
