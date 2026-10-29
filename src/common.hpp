#pragma once

#include <Geode/Geode.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fw {

// одно событие макроса (только кнопка прыжка)
struct MacroEvent {
    uint64_t frame = 0;
    bool down = false; // true = нажатие, false = отпускание
    bool p2 = false;   // событие для второго игрока
};

// результат анализа одного клика (нажатия)
struct ClickInfo {
    uint64_t frame = 0;
    bool p2 = false;
    bool valid = false; // false = даже оригинальный клик не прошёл (рассинхрон)
    int early = 0;      // на сколько кадров РАНЬШЕ можно нажать
    int late = 0;       // на сколько кадров ПОЗЖЕ можно нажать
    float x = 0.f;      // позиция игрока в момент клика
    float y = 0.f;
    int window() const { return early + late + 1; }
};

constexpr int kBuckets = 10;

// 1,2,3,4,5-6,7-8,9-12,13-16,17-19,20+
int bucketIndex(int window);
const char* bucketName(int index);

geode::Result<std::vector<MacroEvent>> loadMacro(std::filesystem::path const& path);
std::filesystem::path configuredMacroPath();

// кэш результатов, чтобы не считать заново
void saveResults(std::filesystem::path const& macroPath, std::vector<ClickInfo> const& results);
bool loadResults(std::filesystem::path const& macroPath, std::vector<ClickInfo>& results);

} // namespace fw
