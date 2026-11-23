#include "Panel.hpp"

#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>

#include <climits>
#include <ctime>
#include <map>
#include <unordered_set>

#include "Overlays.hpp"
#include "core/FileIO.hpp"
#include "core/Formats.hpp"
#include "core/Session.hpp"
#include "game/Bridge.hpp"
#include "game/Jobs.hpp"

using namespace geode::prelude;
namespace G = gdfwr::game;

namespace gdfwr::ui {
namespace {

G::State& S() { return G::State::get(); }
void refreshPanel();  // defined after the panel class

std::tm localNow() { return G::localTime(); }

// Part 8, 8.3: anything that would replace an unsaved document asks first.
void ensureSaved(std::function<void()> proceed) {
    if (!S().file || !S().dirty) return proceed();
    createQuickPopup("Unsaved recording", "The current recording has unsaved changes.\nCancel and use Save first, or discard it.",
                     "Cancel", "Discard", [proceed](FLAlertLayer*, bool discard) { if (discard) proceed(); });
}

void pickFile(file::PickMode mode, std::string const& desc, std::vector<std::string> const& exts,
              std::filesystem::path const& def, std::function<void(std::filesystem::path)> cb) {
    file::FilePickOptions o;
    o.defaultPath = def;
    std::unordered_set<std::string> patterns;
    for (auto const& e : exts) patterns.insert("*" + e);
    o.filters.push_back({desc, patterns});
    async::spawn(file::pick(mode, o), [cb](Result<std::optional<std::filesystem::path>> r) {
        if (r.isErr()) return G::alertBox("File dialog", "The file dialog failed.");
        auto p = r.unwrap();
        if (p) cb(*p);  // empty = the user cancelled: nothing happens
    });
}

// ---- document operations ------------------------------------------------------------------------------------------
void writeTo(std::filesystem::path const& p) {
    auto r = gdfwr::saveAtomic(p, *S().file);
    if (!r.ok) return G::alertBox("Could not save", r.errorText);  // SAVE_WRITE_FAILED: the old file is unchanged
    S().path = p;
    S().dirty = false;
    G::notify("Saved: " + p.filename().string());
    refreshPanel();
}
void doSave(bool asNew) {
    if (!S().file) return;
    if (!asNew && !S().path.empty()) return writeTo(S().path);
    auto def = G::recordingsDir() / gdfwr::defaultFileName(S().file->level.name, localNow(), true, ".gdfwr");
    pickFile(file::PickMode::SaveFile, "GDFWR recording", {".gdfwr"}, def, [](std::filesystem::path p) {
        if (p.extension() != ".gdfwr") p += ".gdfwr";
        writeTo(p);
    });
}

void loadFrom(std::filesystem::path const& p) {
    std::vector<uint8_t> bytes;
    if (!gdfwr::readWholeFile(p, bytes)) return G::alertBox("Load", "The file could not be read.");
    auto r = gdfwr::parse(bytes.data(), bytes.size());
    if (!r.ok) return G::alertBox("Load", G::describeCode(r.errorCode) + "\n" + r.errorText);
    std::vector<gdfwr::Warning> warn = r.loadWarnings;
    if (auto* pl = PlayLayer::get()) {  // Part 8, 8.5.1 step 6
        auto m = gdfwr::compareLevel(r.file.level, true, pl->m_level->m_levelID.value(), G::levelHash(pl->m_level));
        gdfwr::applyLevelMatch(r.file, m, warn);
    }
    S().file = std::move(r.file);
    S().path = p;
    S().dirty = false;  // Load never marks a file VERIFIED by itself
    if (!warn.empty()) {
        std::string t;
        for (auto const& w : warn) t += "- " + G::describeCode(w.code) + "\n";
        G::alertBox("Loaded with warnings", t);
    } else {
        G::notify("Loaded: " + p.filename().string());
    }
    refreshPanel();
}

void runAnalyze();  // defined below
void finishImport(gdfwr::ImportResult res, std::filesystem::path const& src) {
    if (!res.ok) return G::alertBox("Import refused", G::describeCode(res.errorCode) + "\n" + res.errorText);
    S().file = std::move(res.file);
    S().path.clear();
    S().dirty = true;  // an import is a new, unsaved document
    std::string t = "Imported " + std::to_string(S().file->macro.events.size()) + " events from " + src.filename().string() + ".\n";
    for (auto const& w : S().file->header.warnings) t += "- " + G::describeCode(w.code) + "\n";
    t += "\nThe file is NOT_VERIFIED. Analyze it now? Analyze checks the macro first, then measures every click.";
    refreshPanel();
    createQuickPopup("Import finished", t, "Later", "Analyze", [](FLAlertLayer*, bool yes) {
        if (yes) runAnalyze();  // 8.6.8: the verification is step 1 of Analyze, there is no separate Verify
    });
}
void importFrom(std::filesystem::path const& p) {
    auto* pl = PlayLayer::get();
    if (!pl) return G::alertBox("Import", "Open the level first.");
    std::vector<uint8_t> bytes;
    if (!gdfwr::readWholeFile(p, bytes)) return G::alertBox("Import", "The file could not be read.");
    auto const* reader = gdfwr::detectReader(bytes.data(), bytes.size(), p.extension().string());
    if (!reader) return G::alertBox("Import", G::describeCode("IMPORT_UNKNOWN_FORMAT"));  // content first, extension second
    auto parsed = reader->parse(bytes.data(), bytes.size());
    if (!parsed.ok) return G::alertBox("Import", G::describeCode(parsed.errorCode) + "\n" + parsed.errorText);
    gdfwr::ImportOptions o;
    o.haveLoadedLevel = true;
    o.levelId = pl->m_level->m_levelID.value();
    o.levelName = std::string(pl->m_level->m_levelName);
    o.levelHash = G::levelHash(pl->m_level);
    o.levelVersion = pl->m_level->m_levelVersion;
    o.levelTwoPlayer = G::isTwoPlayer(pl);
    o.gameVersion = Loader::get()->getGameVersion();
    o.modVersion = GDFWR_VERSION;
    o.sourceFileName = p.filename().string();
    o.nowUtc = std::time(nullptr);
    auto res = gdfwr::importMacro(parsed.macro, o);
    if (!res.ok && res.askLevelMismatch) {  // 8.6.6: ask again, then flag the file LEVEL_MISMATCH
        auto macro = parsed.macro;
        createQuickPopup("Different level", "This macro was recorded on a different level.\nImport it anyway?", "Cancel", "Import anyway",
                         [macro, o, p](FLAlertLayer*, bool yes) mutable {
                             if (!yes) return;
                             o.acceptLevelMismatch = true;
                             finishImport(gdfwr::importMacro(macro, o), p);
                         });
        return;
    }
    finishImport(std::move(res), p);
}

void doExport() {
    if (!S().file) return;
    bool mismatch = false;
    if (auto* pl = PlayLayer::get())
        mismatch = gdfwr::compareLevel(S().file->level, true, pl->m_level->m_levelID.value(), G::levelHash(pl->m_level)) == gdfwr::LevelMatch::Mismatch;
    std::string t = "GDR2 stores the final run only. Not written:\n";
    for (auto const& s : gdfwr::exportLossList(*S().file)) t += "- " + s + "\n";
    for (auto const& w : gdfwr::exportWarnings(*S().file, mismatch)) t += "! " + w.code + ": " + w.detail + "\n";
    createQuickPopup("Export to GDR2", t, "Cancel", "Export", [](FLAlertLayer*, bool yes) {
        if (!yes) return;
        std::string base = gdfwr::defaultFileName(S().file->level.name, localNow(), false, ".gdr2");
        pickFile(file::PickMode::SaveFile, "GDR2 macro", {".gdr2"}, G::recordingsDir() / base, [](std::filesystem::path p) {
            if (p.extension().empty()) p += ".gdr2";
            auto bytes = gdfwr::exportGdr2(*S().file, 0);  // frame offset: per format, from test I-5 (D9); 0 until measured
            std::string err;
            if (!gdfwr::writeAtomicBytes(p, bytes, true, &err)) return G::alertBox("Export failed", err);
            auto chk = gdfwr::exportCheck(*S().file, bytes);
            if (!chk.empty()) return G::alertBox("Export check failed", "EXPORT_CHECK_FAILED: the written file does not read back correctly.");
            G::notify("Exported: " + p.filename().string());
        });
    });
}

// ---- verify / analyze flow (Part 5) -----------------------------------------------------------------------------------
struct Flow {
    PlayLayer* pl = nullptr;
    std::unique_ptr<G::GameLock> lock;
    std::unique_ptr<G::GameSimulator> sim;
    std::unique_ptr<G::VerifyJob> verify;
    std::unique_ptr<G::AnalyzeJob> job;
    std::unique_ptr<G::StreamedAnalyzeJob> sjob;   // v2: the analysis in chunks (bounded memory), see VerifyJob::needsStreaming
    bool streamed = false;
    gdfwr::Baseline partial;  // after a failed verification (for "Continue")
    gdfwr::Baseline kept;     // baseline the analysis compares against
    gdfwr::TraceReport report;   // v0.3.2 (Phase 2): the silent replay against the real playback of the same macro (empty when the setting is off)
    bool haveReport = false;
};

void endFlow(std::shared_ptr<Flow> f) {
    f->sjob.reset();
    f->job.reset();
    f->verify.reset();
    f->lock->release(true);  // put the game back: level restarted, practice mode as before, own update loop resumed
}

// v0.3.2 (Phase 2, MEASUREMENT ONLY): compares the silent replay that just ended with the last real playback of the same macro, saves the numbers
// as two .csv files next to the recordings and writes everything to the Geode log (lines "GDFWR-TRACE ..."). Nothing is moved or changed.
void buildComparison(Flow& f) {
    f.report = {};
    f.haveReport = false;
    if (!f.verify || !Mod::get()->getSettingValue<bool>("trace-playback")) return;
    f.report = G::makeTraceReport(*f.verify);
    f.haveReport = true;
    std::string err;
    auto files = G::writeTraceFiles(f.report, f.verify->fileRef().level.name, G::recordingsDir(), err);
    std::vector<std::string> extra;
    if (!files.empty()) {
        std::string line = "The numbers are saved as csv files (open them in a spreadsheet): ";
        for (size_t i = 0; i < files.size(); i++) line += std::string(i ? ", " : "") + files[i].filename().string();
        extra.push_back(line);
        extra.push_back("Folder: " + G::recordingsDir().string());
    }
    if (!err.empty()) extra.push_back("The csv files could not be written: " + err);
    for (auto const& l : extra) { f.report.popup.push_back(l); f.report.result.push_back(l); f.report.log.push_back(l); }
    G::logTraceLines(f.report.log);
}

// The text of a failed check: the message, then the comparison with the real playback (when it was measured), then the question. The comparison is long,
// so it needs a popup that scrolls; without it (setting off) the old popup is used unchanged.
void showFailure(std::shared_ptr<Flow> f, char const* title, std::string const& msg, std::string const& ask, char const* no, char const* yes,
                 std::function<void(bool)> answer) {
    if (!f->haveReport) {
        createQuickPopup(title, msg + "\n\n" + ask, no, yes, [answer](FLAlertLayer*, bool b) { answer(b); });
        return;
    }
    std::vector<std::string> rows;
    size_t pos = 0;
    while (pos <= msg.size()) {
        size_t nl = msg.find('\n', pos);
        rows.push_back(msg.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    rows.push_back("");
    for (auto const& l : f->report.popup) rows.push_back(l);
    rows.push_back("");
    rows.push_back(ask);
    ChoicePopup::create(title, rows, no, yes, std::move(answer))->show();
}

void startAnalyze(std::shared_ptr<Flow> f, int until) {
    f->job = std::make_unique<G::AnalyzeJob>(f->pl, *S().file, *f->sim, f->kept, until, std::vector<int>{});
    ProgressPopup::Hooks h;
    h.pump = [f](double ms) { return f->job->pump(ms); };
    h.status = [f]() {
        auto lv = f->job->engine().live();
        auto const& k = lv.counters;
        char b[256];
        std::snprintf(b, sizeof b,
                      "Action %d of %zu\nearly: %s   late: %s\n1:%d 2:%d 3:%d 4:%d 5-6:%d 7-10:%d 11-24:%d 25+:%d  N+:%d CLAMP:%d UNRELIABLE:%d RANDOM:%d",
                      lv.action, f->job->engine().result().clicks.size() * 2, lv.mapEarly.c_str(), lv.mapLate.c_str(), k.b1, k.b2, k.b3, k.b4, k.b5_6,
                      k.b7_10, k.b11_24, k.b25p, k.nPlus, k.clamp, k.unreliable, k.random);
        return std::string(b);
    };
    h.progress = [f]() {
        auto p = f->job->engine().progress();
        return p.total ? float(p.done) / float(p.total) : 0.f;
    };
    h.cancel = [f]() { f->job->cancel(); };
    h.done = [f]() {
        S().file = f->job->file();
        S().dirty = true;
        endFlow(f);
        if (S().file->analysis) ResultPopup::create(*S().file, f->report.result)->show();   // v0.3.0: it also lists the clicks the fit moved
    };
    ProgressPopup::create("Analyze 2/2: measuring the clicks", std::move(h))->show();
}

// v2: a macro with more click edges than "Max saved states" is measured in chunks of clicks; each chunk replays the level again and keeps
// only its own states. Same result, bounded memory (important on phones), a little more time.
void startAnalyzeStreamed(std::shared_ptr<Flow> f, int until) {
    f->sjob = std::make_unique<G::StreamedAnalyzeJob>(f->pl, *S().file, *f->sim, f->kept, until);
    ProgressPopup::Hooks h;
    h.pump = [f](double ms) { return f->sjob->pump(ms); };
    h.status = [f]() { return f->sjob->statusText(); };
    h.progress = [f]() { return f->sjob->progress(); };
    h.cancel = [f]() { f->sjob->cancel(); };
    h.done = [f]() {
        bool failed = f->sjob->failed();
        std::string why = f->sjob->message();
        S().file = f->sjob->file();
        S().dirty = true;
        endFlow(f);
        if (S().file->analysis) ResultPopup::create(*S().file, f->report.result)->show();
        if (failed) G::alertBox("Analysis stopped", why);
    };
    ProgressPopup::create("Analyze 2/2: measuring the clicks (in parts)", std::move(h))->show();
}

void startAnalyzeAuto(std::shared_ptr<Flow> f, int until) {
    if (f->streamed) startAnalyzeStreamed(f, until);
    else startAnalyze(f, until);
}

void startVerify(std::shared_ptr<Flow> f);

void onVerifyDone(std::shared_ptr<Flow> f) {
    auto& v = *f->verify;
    if (v.cancelled()) return endFlow(f);
    f->streamed = v.needsStreaming();
    S().file = v.file();  // VERIFIED or FAILED(tick) is stored in the document (and the shifted clicks, when the repair worked)
    S().dirty = true;
    // v0.3.2 (Phase 2): where does the silent replay differ from the real playback? Only when the silent replay really ran (a probe that never moved the
    // player has nothing to compare).
    if (v.ok() || v.failKind() == G::FailKind::Death || v.failKind() == G::FailKind::NoEnd) buildComparison(*f);
    if (v.ok()) {
        f->kept = *S().file->baseline;
        refreshPanel();
        if (!v.repairs().empty()) {
            // The Fit is on (v0.3.1: it is off by default) and the silent replay only passed after clicks were MOVED. That changed the
            // macro; the result screen starts with a warning and lists every moved click (recorded -> now), and Edit clicks > Restore all
            // brings the recorded ticks back.
            G::notify("Fit MOVED " + std::to_string(v.repairs().size()) + " click(s) of your macro: see the result (Edit clicks > Restore all undoes it)", false);
        }
        return startAnalyzeAuto(f, INT_MAX);  // the macro replays to the end of the level: now measure it
    }
    f->partial = v.baseline();
    f->kept = f->partial;
    int tick = v.failTick();
    std::string msg = v.message();
    auto kind = v.failKind();
    refreshPanel();
    if (kind == G::FailKind::Death) {
        // Part 5, step 3: the user may still analyse the clicks that come BEFORE the failure. v0.3.1: the message says that it is the
        // silent replay that died and that the macro was not changed.
        showFailure(f, "Silent replay failed", msg, "Analyse only the clicks before tick " + std::to_string(tick) + "? (Your macro stays as it is.)", "No", "Continue",
                    [f, tick](bool cont) { if (cont) startAnalyzeAuto(f, tick); else endFlow(f); });
    } else if (kind == G::FailKind::NoEnd) {
        // The macro replayed without dying, so every click can still be measured; only the "reached the end" proof is missing.
        showFailure(f, "Verification failed", msg, "The file stays NOT verified. Measure the clicks anyway?", "No", "Measure",
                    [f, tick](bool cont) { if (cont) startAnalyzeAuto(f, tick); else endFlow(f); });
    } else {
        endFlow(f);
        G::alertBox("Verification failed", msg);
    }
}

void startVerify(std::shared_ptr<Flow> f) {
    f->lock = std::make_unique<G::GameLock>(f->pl);   // also closes the pause menu: a paused game does not run
    f->sim = std::make_unique<G::GameSimulator>(f->pl);
    auto* mod = Mod::get();
    // v0.3.1: "fit-clicks" (default OFF) replaces "auto-repair" (default on): a Fit moves clicks until the SILENT replay passes, which made
    // a macro that passes in normal playback die at its spike. The new name means an old saved "on" is not read.
    bool fit = mod->getSettingValue<bool>("fit-clicks");
    f->verify = std::make_unique<G::VerifyJob>(f->pl, *S().file, *f->sim, fit, int(mod->getSettingValue<int64_t>("repair-max-shift")));
    ProgressPopup::Hooks h;
    h.pump = [f](double ms) { return f->verify->pump(ms); };
    h.status = [f]() { return f->verify->statusText(); };
    h.progress = [f]() { return f->verify->progress(); };
    h.cancel = [f]() { f->verify->cancel(); };
    h.done = [f]() { onVerifyDone(f); };
    ProgressPopup::create(fit ? "Analyze 1/2: replaying the macro (and fitting its clicks)" : "Analyze 1/2: replaying the macro (nothing is moved)", std::move(h))->show();
}

// Why Analyze is not possible right now (empty = it is). One place for the button state and for the "Analyze now?" after an import.
std::string analyzeReason() {
    auto& s = S();
    auto* pl = PlayLayer::get();
    if (s.session.unfinished()) return "Stop recording first.";
    if (s.sim.active) return "An analysis is running.";
    if (!s.file) return "No recording is open.";
    if (!s.file->macro.finalRun.exists) return "This recording has no final run.";
    if (!pl) return "Open the level first.";
    auto lm = gdfwr::compareLevel(s.file->level, true, pl->m_level->m_levelID.value(), G::levelHash(pl->m_level));
    if (lm == gdfwr::LevelMatch::Mismatch) return "The file belongs to a different level (LEVEL_MISMATCH).";
    return gdfwr::analysisGate(*s.file, true);
}

// Analyze is ONE process (Part 5): 1. check the macro from start to end (the verification pass, which also saves the game states
// the analysis starts from), 2. measure every click. There is no separate Verify control (Appendix A, item 8).
void runAnalyze() {
    if (!PlayLayer::get() || !S().file || S().sim.active) return;
    if (auto why = analyzeReason(); !why.empty()) return G::alertBox("Analyze", why);
    // v0.3.2: a playback that is still running ends here. Its trace is kept (it is what the silent replay is compared with), and the replay is not
    // left half-played behind the analysis.
    if (S().replay.active) G::stopReplay("Replay stopped: Analyze was started.", gdfwr::TraceEnd::Analyze);
    auto go = [] {
        auto* pl = PlayLayer::get();
        if (!pl || !S().file) return;
        auto f = std::make_shared<Flow>();
        f->pl = pl;
        startVerify(f);
    };
    // v0.3.0: no question first. The windows are measured by replaying the level, so a recording that is only exact to `res` ticks (no
    // click-between-frames) is fine (COARSE_INPUT is listed in the result). v0.3.1: the clicks are only moved when the Fit setting is on.
    int res = S().file->timing.clickResolution;
    if (res > 1) {
        bool fit = Mod::get()->getSettingValue<bool>("fit-clicks");
        G::notify("Clicks are only exact to " + std::to_string(res) + " ticks: " + (fit ? "the replay will fit them to the silent replay" : "they are replayed as recorded, nothing is moved"));
    }
    go();
}

// ---- recording start / pause ---------------------------------------------------------------------------------------------
void startRecording() {
    auto* pl = PlayLayer::get();
    if (!pl) return;
    S().file.reset();  // the old document was saved or discarded by ensureSaved()
    S().path.clear();
    S().dirty = false;
    S().pendingStart = true;
    G::closePauseMenu();
    if (!pl->m_isPracticeMode) pl->togglePracticeMode(true);  // Part 1, 3.1: Record enables Practice Mode
    pl->removeAllCheckpoints();
    pl->resetLevelFromStart();  // the reset hook starts segment 0 at tick 0
    if (S().pendingStart) {
        S().pendingStart = false;
        G::alertBox("Record", "The level could not be restarted.");
    }
    refreshPanel();
}
void beginRecordingFlow() {
    auto* pl = PlayLayer::get();
    if (!pl) return G::alertBox("Record", "Open a level first.");
    auto why = G::unsupportedReason(pl);
    if (!why.empty()) return G::alertBox("Record", why);
    if (S().session.unfinished() || S().sim.active) return;
    ensureSaved([] { startRecording(); });
}

// ---- click editor (Part 1, 3.5) ---------------------------------------------------------------------------------------------
class EditPopup : public Popup {
public:
    static EditPopup* create() {
        auto r = new EditPopup();
        if (r->init()) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }
protected:
    bool init() {
        auto win = CCDirector::get()->getWinSize();
        float w = win.width, h = win.height;
        if (!Popup::init(w, h)) return false;
        this->setTitle("Edit clicks");
        m_info = CCLabelBMFont::create("", "chatFont.fnt");
        m_info->setScale(0.7f);
        m_mainLayer->addChildAtPosition(m_info, Anchor::BottomLeft, {w * 0.5f, h * 0.62f}, false);
        auto mk = [&](int tag, char const* txt, float fx, float fy) {
            auto b = CCMenuItemSpriteExtra::create(ButtonSprite::create(txt, "bigFont.fnt", "GJ_button_01.png", .7f), this, menu_selector(EditPopup::onBtn));
            b->setTag(tag);
            m_buttonMenu->addChildAtPosition(b, Anchor::BottomLeft, {w * fx, h * fy});
        };
        mk(1, "<", 0.30f, 0.50f); mk(2, ">", 0.70f, 0.50f);
        mk(3, "-10", 0.22f, 0.38f); mk(4, "-1", 0.38f, 0.38f); mk(5, "+1", 0.62f, 0.38f); mk(6, "+10", 0.78f, 0.38f);
        mk(7, "Restore", 0.40f, 0.24f); mk(8, "Restore all", 0.62f, 0.24f);
        auto close = CCMenuItemSpriteExtra::create(ButtonSprite::create("Close", "bigFont.fnt", "GJ_button_01.png", .8f), this, menu_selector(EditPopup::onClose));
        m_buttonMenu->addChildAtPosition(close, Anchor::Bottom, {0.f, h * 0.07f});
        m_ev = gdfwr::finalRunEvents(S().file->macro);
        this->show1("Pick an event with < and >, move it with the tick buttons.");
        return true;
    }
    void show1(std::string const& msg) {
        std::string t = msg;
        if (!m_ev.empty()) {
            auto const& e = m_ev[size_t(m_sel)];
            auto const* real = gdfwr::findEvent(S().file->macro, e.seq);
            char b[200];
            std::snprintf(b, sizeof b, "Event %d of %zu:  P%d %s  tick %d  (recorded %d)%s", m_sel + 1, m_ev.size(), e.player + 1,
                          e.type == gdfwr::EventType::Press ? "PRESS" : "RELEASE", e.editedTick, real ? real->tick : e.tick,
                          (e.flags & gdfwr::EventFlag::Synthetic) ? "  [added by the mod]" : "");
            t = std::string(b) + "\n" + msg;
        }
        m_info->setString(t.c_str());
    }
    void onBtn(CCObject* s) {
        int tag = static_cast<CCNode*>(s)->getTag();
        auto& f = *S().file;
        if (m_ev.empty()) return;
        auto const seq = m_ev[size_t(m_sel)].seq;
        std::string msg;
        if (tag == 1) m_sel = std::max(0, m_sel - 1);
        else if (tag == 2) m_sel = std::min(int(m_ev.size()) - 1, m_sel + 1);
        else if (tag >= 3 && tag <= 6) {
            static const int d[] = {-10, -1, 1, 10};
            auto r = gdfwr::moveEvent(f.macro, seq, m_ev[size_t(m_sel)].editedTick + d[tag - 3]);
            if (!r.ok) msg = r.error;
            else { gdfwr::applyEditSideEffects(f); S().dirty = true; msg = "Moved. The file is NOT_VERIFIED now and the old analysis is STALE."; }
        } else if (tag == 7) {
            auto r = gdfwr::restoreEvent(f.macro, seq);
            if (!r.ok) msg = r.error;
            else { gdfwr::applyEditSideEffects(f); S().dirty = true; }
        } else if (tag == 8) {
            gdfwr::restoreAll(f.macro);
            gdfwr::applyEditSideEffects(f);
            S().dirty = true;
        }
        m_ev = gdfwr::finalRunEvents(f.macro);
        m_sel = std::clamp(m_sel, 0, std::max(0, int(m_ev.size()) - 1));
        this->show1(msg);
        refreshPanel();
    }
    std::vector<gdfwr::Event> m_ev;
    int m_sel = 0;
    CCLabelBMFont* m_info = nullptr;
};

// ---- the panel ------------------------------------------------------------------------------------------------------------------
enum Tag { tRecord = 1, tStop, tPause, tAnalyze, tReplay, tEdit, tSave, tSaveAs, tLoad, tImport, tExport };
class GdfwrPanel;
GdfwrPanel* s_panel = nullptr;  // the open panel, cleared by its destructor

class GdfwrPanel : public Popup {
public:
    ~GdfwrPanel() override { if (s_panel == this) s_panel = nullptr; }
    static GdfwrPanel* create() {
        auto r = new GdfwrPanel();
        if (r->init()) { r->autorelease(); s_panel = r; return r; }
        delete r;
        return nullptr;
    }
    void refresh() {
        auto& s = S();
        auto* pl = PlayLayer::get();
        bool rec = s.session.unfinished();
        std::array<std::string, 5> l;
        if (s.file) l[0] = "File: " + (s.path.empty() ? std::string("(not saved yet)") : s.path.filename().string()) + (s.dirty ? "    * unsaved changes" : "");
        else l[0] = "File: none";
        if (s.file) {
            l[1] = "Level: " + s.file->level.name + "  [" + std::to_string(s.file->level.levelId) + "]";
            if (pl) {
                auto m = gdfwr::compareLevel(s.file->level, true, pl->m_level->m_levelID.value(), G::levelHash(pl->m_level));
                if (m == gdfwr::LevelMatch::Mismatch) l[1] += "    LEVEL_MISMATCH";
                if (m == gdfwr::LevelMatch::Changed) l[1] += "    LEVEL_CHANGED";
            }
        } else if (pl) l[1] = "Level: " + std::string(pl->m_level->m_levelName);
        else l[1] = "Level: none loaded";
        if (rec) {
            l[2] = "Recording: ACTIVE";
            l[3] = std::to_string(s.session.liveActionCount()) + " actions so far";
        } else if (s.file) {
            auto const& h = s.file->header;
            char b[200];
            std::snprintf(b, sizeof b, "Recording: %s    Verification: %s%s    Edit: %s    Origin: %s", h.rec == gdfwr::RecStatus::Completed ? "COMPLETED" : "PARTIAL",
                          h.verify == gdfwr::VerifyStatus::Verified ? "VERIFIED" : h.verify == gdfwr::VerifyStatus::Failed ? "FAILED" : "NOT_VERIFIED",
                          h.verify == gdfwr::VerifyStatus::Failed ? (" (tick " + std::to_string(h.verifyFailTick) + ")").c_str() : "",
                          h.edit == gdfwr::EditStatus::Edited ? "EDITED" : "ORIGINAL", h.origin == gdfwr::Origin::Imported ? "IMPORTED" : "RECORDED");
            l[2] = b;
            l[3] = std::string("Analysis: ") + gdfwr::analysisStateName(*s.file) + "    Actions: " + std::to_string(gdfwr::countActions(s.file->macro));
        }
        if (s.file && !s.file->header.warnings.empty()) l[4] = "! " + G::describeCode(s.file->header.warnings[0].code) + (s.file->header.warnings.size() > 1 ? "  (+" + std::to_string(s.file->header.warnings.size() - 1) + " more)" : "");
        for (size_t i = 0; i < l.size(); i++) m_line[i]->setString(l[i].c_str());
        for (auto& [tag, btn] : m_btn) btn->setNormalImage(spriteFor(tag));
    }
protected:
    bool init() {
        auto win = CCDirector::get()->getWinSize();
        float w = win.width, h = win.height;
        if (!Popup::init(w, h)) return false;  // fills the whole screen (Part 10)
        this->setTitle(std::string("GDFWR - Frame Window Recorder  v") + GDFWR_VERSION);
        for (size_t i = 0; i < m_line.size(); i++) {
            m_line[i] = CCLabelBMFont::create("", "chatFont.fnt");
            m_line[i]->setScale(0.55f);
            m_line[i]->setAnchorPoint({0.f, 1.f});
            m_mainLayer->addChildAtPosition(m_line[i], Anchor::BottomLeft, {w * 0.04f, h * (0.87f - 0.05f * float(i))}, false);
        }
        struct Col { char const* title; float x; std::vector<int> tags; };
        std::vector<Col> cols = {{"Recorder", 0.17f, {tRecord, tStop, tPause}},
                                 {"Analysis", 0.50f, {tAnalyze, tReplay, tEdit}},
                                 {"File", 0.83f, {tSave, tSaveAs, tLoad, tImport, tExport}}};
        for (auto& c : cols) {
            auto t = CCLabelBMFont::create(c.title, "goldFont.fnt");
            t->setScale(0.6f);
            m_mainLayer->addChildAtPosition(t, Anchor::BottomLeft, {w * c.x, h * 0.60f}, false);
            for (size_t i = 0; i < c.tags.size(); i++) {
                int tag = c.tags[i];
                auto btn = CCMenuItemSpriteExtra::create(spriteFor(tag), this, menu_selector(GdfwrPanel::onBtn));
                btn->setTag(tag);
                m_buttonMenu->addChildAtPosition(btn, Anchor::BottomLeft, {w * c.x, h * (0.52f - 0.095f * float(i))});
                m_btn[tag] = btn;
            }
        }
        auto close = CCMenuItemSpriteExtra::create(ButtonSprite::create("Close", "bigFont.fnt", "GJ_button_01.png", .8f), this, menu_selector(GdfwrPanel::onClose));
        m_buttonMenu->addChildAtPosition(close, Anchor::Bottom, {0.f, h * 0.07f});
        this->refresh();
        return true;
    }
    void onBtn(CCObject* sender) {
        int tag = static_cast<CCNode*>(sender)->getTag();
        if (auto why = reasonFor(tag); !why.empty()) return G::alertBox("Not available", why);  // disabled = grey, and tapping says why (8.3)
        switch (tag) {
            case tRecord: this->onClose(nullptr); beginRecordingFlow(); return;
            case tStop: G::finishRecording(PlayLayer::get(), gdfwr::EndReason::Stopped); break;
            case tPause: togglePauseRecording(); break;
            case tAnalyze: this->onClose(nullptr); runAnalyze(); return;
            case tReplay: {
                std::string err;
                if (!G::startReplay(PlayLayer::get(), err)) return G::alertBox("Replay", err);
                this->onClose(nullptr);
                if (!S().replay.plan.numbers) G::notify(S().replay.plan.note, false);   // e.g. "No analysis yet: the replay shows the macro only."
                return;
            }
            case tEdit: EditPopup::create()->show(); return;
            case tSave: doSave(false); break;
            case tSaveAs: doSave(true); break;
            case tLoad: pickFile(file::PickMode::OpenFile, "GDFWR recording", {".gdfwr"}, G::recordingsDir(), [](std::filesystem::path p) { ensureSaved([p] { loadFrom(p); }); }); break;
            case tImport: pickFile(file::PickMode::OpenFile, "Macro files", {".gdr2", ".gdr", ".xd"}, {}, [](std::filesystem::path p) { ensureSaved([p] { importFrom(p); }); }); break;
            case tExport: doExport(); break;
        }
        this->refresh();
    }
private:
    std::string labelFor(int tag) {
        switch (tag) {
            case tRecord: return "Record";
            case tStop: return "Stop";
            case tPause: return S().session.state() == gdfwr::SessionModel::State::Active ? "Pause Rec" : "Resume Rec";
            case tAnalyze: return "Analyze";
            case tReplay: return "Replay";
            case tEdit: return "Edit clicks";
            case tSave: return "Save";
            case tSaveAs: return "Save As";
            case tLoad: return "Load";
            case tImport: return "Import";
            default: return "Export";
        }
    }
    ButtonSprite* spriteFor(int tag) {
        return ButtonSprite::create(labelFor(tag).c_str(), "bigFont.fnt", reasonFor(tag).empty() ? "GJ_button_01.png" : "GJ_button_04.png", .7f);
    }
    // Part 8, 8.3: the reason why a button is off (empty = enabled)
    std::string reasonFor(int tag) {
        auto& s = S();
        auto* pl = PlayLayer::get();
        bool rec = s.session.unfinished(), has = s.file.has_value();
        bool final = has && s.file->macro.finalRun.exists;
        switch (tag) {
            case tRecord: return rec ? "Already recording." : s.sim.active ? "An analysis is running." : G::unsupportedReason(pl);
            case tStop: return rec ? "" : "Nothing is being recorded.";
            case tPause: return rec ? "" : "Nothing is being recorded.";
            case tAnalyze: return analyzeReason();
            case tReplay: {   // a replay only needs a macro: no analysis, no verification (so the user can watch whether it passes)
                if (rec || !pl) return rec ? "Stop recording first." : "Open the level first.";
                if (s.sim.active) return "An analysis is running.";
                if (!has) return "There is no macro. Record or load one first.";
                return gdfwr::buildReplayPlan(*s.file).reason;
            }
            case tEdit: return rec ? "Stop recording first." : final ? "" : "No final run to edit.";
            case tSave: case tSaveAs: case tExport: return rec ? "Stop recording first." : final ? "" : "No recording is open.";
            case tLoad: return rec ? "Stop recording first." : "";
            case tImport: return rec ? "Stop recording first." : pl ? "" : "Open the level first.";
        }
        return {};
    }
    std::map<int, CCMenuItemSpriteExtra*> m_btn;
    std::array<CCLabelBMFont*, 5> m_line{};
};

void refreshPanel() {
    if (s_panel) s_panel->refresh();
}

}  // namespace

void openPanel() {
    if (S().sim.active) return;
    if (s_panel) return;
    if (auto* p = GdfwrPanel::create()) p->show();
}
void toggleRecording() {
    if (S().sim.active) return;
    if (S().session.unfinished()) G::finishRecording(PlayLayer::get(), gdfwr::EndReason::Stopped);
    else beginRecordingFlow();
    refreshPanel();
}
void togglePauseRecording() {
    auto* pl = PlayLayer::get();
    using St = gdfwr::SessionModel::State;
    if (S().session.state() == St::Active) { S().session.suspend(pl ? G::tickOf(pl) : S().lastTick); G::notify("Recording paused: nothing is captured until you resume."); }
    else if (S().session.state() == St::Suspended) { S().session.arm(); G::notify("Recording armed: restart the level to continue capturing."); }
    refreshPanel();
}

}  // namespace gdfwr::ui
