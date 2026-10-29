#include "Common.hpp"

#include <gdr/gdr.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

using namespace geode::prelude;

namespace {
    struct FWReplay : gdr::Replay<FWReplay, gdr::Input<>> {
        FWReplay() : Replay("FrameWindows", 1) {}
    };

    int maxShiftSetting() {
        return static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-shift"));
    }
    int lookaheadSetting() {
        return static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead"));
    }

    std::filesystem::path cachePath(std::filesystem::path const& macroPath) {
        auto dir = Mod::get()->getSaveDir() / "analysis";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / (macroPath.stem().string() + ".txt");
    }

    uint64_t fileSize(std::filesystem::path const& p) {
        std::error_code ec;
        auto s = std::filesystem::file_size(p, ec);
        return ec ? 0 : static_cast<uint64_t>(s);
    }
}

namespace fw {

int bucketIndex(int w) {
    if (w <= 1) return 0;
    if (w == 2) return 1;
    if (w == 3) return 2;
    if (w == 4) return 3;
    if (w <= 6) return 4;
    if (w <= 8) return 5;
    if (w <= 12) return 6;
    if (w <= 16) return 7;
    if (w <= 19) return 8;
    return 9;
}

const char* bucketName(int i) {
    static const char* names[kBuckets] = {
        "1", "2", "3", "4", "5 - 6", "7 - 8", "9 - 12", "13 - 16", "17 - 19", "20+"
    };
    return names[std::clamp(i, 0, kBuckets - 1)];
}

std::filesystem::path configuredMacroPath() {
    return Mod::get()->getSettingValue<std::filesystem::path>("macro-path");
}

Result<std::vector<MacroEvent>> loadMacro(std::filesystem::path const& path) {
    if (path.empty()) return Err("No macro selected (mod settings -> Macro)");

    auto res = FWReplay::importData(path);
    if (res.isErr()) return Err(std::string("Failed to read macro: ") + res.unwrapErr());

    auto const& replay = res.unwrap();
    if (replay.platformer) return Err("Platformer macros are not supported yet");
    if (std::abs(replay.framerate - 240.0) > 0.5) {
        return Err("Only 240 TPS macros are supported");
    }

    std::vector<MacroEvent> out;
    for (auto const& in : replay.inputs) {
        if (in.button != 1) continue; // только прыжок
        MacroEvent e;
        e.frame = in.frame;
        e.down = in.down;
        e.p2 = in.player2;
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](MacroEvent const& a, MacroEvent const& b) {
        return a.frame < b.frame;
    });

    if (out.empty()) return Err("Macro has no clicks");
    return Ok(std::move(out));
}

void saveResults(std::filesystem::path const& macroPath, std::vector<ClickInfo> const& results) {
    std::ofstream f(cachePath(macroPath));
    if (!f) return;
    f << "FW1 " << fileSize(macroPath) << " " << maxShiftSetting() << " " << lookaheadSetting() << "\n";
    for (auto const& c : results) {
        f << c.frame << " " << (c.p2 ? 1 : 0) << " " << (c.valid ? 1 : 0) << " "
          << c.early << " " << c.late << " " << c.x << " " << c.y << "\n";
    }
}

bool loadResults(std::filesystem::path const& macroPath, std::vector<ClickInfo>& results) {
    std::ifstream f(cachePath(macroPath));
    if (!f) return false;

    std::string tag;
    uint64_t size = 0;
    int maxShift = 0, look = 0;
    f >> tag >> size >> maxShift >> look;
    if (tag != "FW1" || size != fileSize(macroPath) || maxShift != maxShiftSetting() || look != lookaheadSetting()) {
        return false;
    }

    std::vector<ClickInfo> out;
    ClickInfo c;
    int p2 = 0, valid = 0;
    while (f >> c.frame >> p2 >> valid >> c.early >> c.late >> c.x >> c.y) {
        c.p2 = p2 != 0;
        c.valid = valid != 0;
        out.push_back(c);
    }
    if (out.empty()) return false;
    results = std::move(out);
    return true;
}

} // namespace fw
