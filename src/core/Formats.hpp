// Import / export of other bots' macro formats (Part 8, 8.6 and 8.7).
// One reader per format: detect / parse / info. All conversion and cleaning is shared (8.6.7).
#pragma once
#include "Model.hpp"

namespace gdfwr {

struct SourceInput {
    uint64_t frame = 0;
    uint8_t button = kJump;  // 1 = jump, 2 = left, 3 = right (platformer)
    bool player2 = false;
    bool down = false;
};
struct SourceMacro {
    std::string formatName, formatVersion;
    double fps = 240;
    std::vector<SourceInput> inputs;  // in source order
    std::optional<int64_t> levelId;
    std::string levelName;
    std::string botName;
    int botVersion = 0;
    std::optional<int64_t> seed;
    int gameVersion = 0;
    bool platformer = false;
    bool startsFromCheckpoint = false;
};
struct ParseResult {
    bool ok = false;
    std::string errorCode, errorText;
    SourceMacro macro;
};
struct FormatInfo {
    std::string name;
    std::vector<std::string> extensions;
    std::string limits;
    bool canExport = false;
    bool enabled = true;
};
class IFormatReader {
public:
    virtual ~IFormatReader() = default;
    virtual bool detect(const uint8_t* d, size_t n) const = 0;
    virtual ParseResult parse(const uint8_t* d, size_t n) const = 0;  // never crashes on damaged data
    virtual FormatInfo info() const = 0;
};

const std::vector<const IFormatReader*>& allReaders();
// content first, extension second (8.6.1). nullptr = IMPORT_UNKNOWN_FORMAT
const IFormatReader* detectReader(const uint8_t* d, size_t n, const std::string& extension);

// ---- import (8.6.4 - 8.6.8) ----
struct ImportOptions {
    int frameOffset = 0;  // D9: per format, set from test I-5
    bool haveLoadedLevel = false;
    int64_t levelId = 0;
    std::string levelName;
    uint64_t levelHash = 0;
    int32_t levelVersion = 0;
    bool levelTwoPlayer = false, levelDual = false;
    std::string gameVersion, modVersion, sourceFileName;
    int64_t nowUtc = 0;
    bool acceptLevelMismatch = false;  // the user confirmed after being asked again (8.6.6)
};
struct ImportResult {
    bool ok = false;
    std::string errorCode, errorText;
    bool askLevelMismatch = false;  // the file stores a level that differs from the loaded one: ask again
    GdfwrFile file;
};
ImportResult importMacro(const SourceMacro& src, const ImportOptions& opt);

// ---- export (8.7) ----
std::vector<uint8_t> exportGdr2(const GdfwrFile& f, int frameOffset);
std::vector<std::string> exportLossList(const GdfwrFile& f);       // 8.7.3, shown BEFORE writing
std::vector<Warning> exportWarnings(const GdfwrFile& f, bool levelMismatch);  // 8.7.1 / 8.7.4
// Re-read with the matching reader and compare the event count (8.7.1). Empty = ok, otherwise EXPORT_CHECK_FAILED.
std::string exportCheck(const GdfwrFile& f, const std::vector<uint8_t>& written);

inline constexpr int kGdr2WriterVersion = 2;  // [VERIFY] value written into the GDR header; readers accept any value

}  // namespace gdfwr
