#pragma once

#include "Common.hpp"
#include <Geode/Geode.hpp>
#include <array>
#include <string>
#include <vector>

namespace fw {

// нажимает/отпускает прыжок так же, как это делает игра при реальном клике
void applyClick(PlayLayer* pl, MacroEvent const& e);

// Считает "окно" каждого клика макроса:
// для каждого нажатия двигаем его на N кадров раньше/позже, прогоняем настоящую физику игры
// (через чекпоинты PlayLayer) и смотрим, умирает ли игрок до конца "горизонта" проверки.
class Analyzer {
public:
    enum class Phase { Idle, Advance, Trials, Done };

    static Analyzer& get();

    // читает макрос + пытается загрузить кэш результатов. Анализ не запускает.
    geode::Result<> load(std::filesystem::path const& macroPath);

    // запускает анализ (уровень должен быть открыт и стоять на паузе)
    geode::Result<> start(PlayLayer* pl, std::filesystem::path const& macroPath);

    // работает не дольше budget секунд. true = закончено (успешно или с ошибкой)
    bool tick(double budgetSeconds);
    void cancel();
    // уровень уничтожается: просто забыть всё, ничего не трогая в игре
    void abandon();

    bool running() const { return m_phase != Phase::Idle; }
    bool simulating() const { return m_simulating; }
    void markDead() { m_died = true; }
    void markEnded() { m_ended = true; }

    float progress() const;
    std::string const& status() const { return m_status; }
    bool lastRunFailed() const { return m_failed; }

    bool hasResults() const { return !m_results.empty(); }
    std::vector<ClickInfo> const& results() const { return m_results; }
    std::vector<MacroEvent> const& events() const { return m_events; }

private:
    bool advanceStep();
    bool trialStep();
    bool runTrial(int shift, bool record);
    bool stepOnce();
    void makeCheckpoint();
    void beginClick();
    void endClick();
    void applyBaseline(MacroEvent const& e);
    void syncHolds(std::array<bool, 2> const& hold);
    void finish(bool ok, std::string const& message);

    Phase m_phase = Phase::Idle;
    bool m_simulating = false;
    bool m_died = false;
    bool m_ended = false;
    bool m_fatal = false;
    bool m_failed = false;
    std::string m_status;
    std::string m_error;

    PlayLayer* m_pl = nullptr;
    std::filesystem::path m_macroPath;
    std::vector<MacroEvent> m_events;
    std::vector<size_t> m_pressIdx; // индексы нажатий в m_events
    std::vector<ClickInfo> m_results;

    size_t m_k = 0;               // номер текущего клика
    uint64_t m_startFrame = 0;    // кадр, где делаем чекпоинт для клика m_k
    size_t m_baseIdx = 0;         // следующее ещё не применённое событие базового прогона
    std::array<bool, 2> m_hold{false, false};

    CheckpointObject* m_cp = nullptr;
    uint64_t m_cpFrame = 0;
    size_t m_cpEventIdx = 0;
    std::array<bool, 2> m_holdAtCp{false, false};

    // параметры текущего клика
    size_t m_pressEvIdx = 0;
    int m_maxLate = 0;
    int m_maxEarly = 0;
    int m_stage = 0;
    int m_lo = 0;
    int m_hi = 0;
    float m_recX = 0.f;
    float m_recY = 0.f;

    int m_maxShift = 24;
    int m_lookahead = 48;
};

} // namespace fw
