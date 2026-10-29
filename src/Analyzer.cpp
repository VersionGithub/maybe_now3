#include "Analyzer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

using namespace geode::prelude;

namespace {
    uint64_t progressOf(PlayLayer* pl) {
        return pl->m_gameState.m_currentProgress;
    }

    bool isHeld(PlayerObject* p) {
        auto it = p->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
        return it != p->m_holdingButtons.end() && it->second;
    }

    void syncOne(PlayerObject* p, bool want) {
        if (!p || isHeld(p) == want) return;
        if (want) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
    }
}

namespace fw {

void applyClick(PlayLayer* pl, MacroEvent const& e) {
    bool swapControls = GameManager::get()->getGameVariable("0010"); // "Flip 2-player controls"
    bool player2 = swapControls ? !e.p2 : e.p2;
    auto action = e.down ? &PlayerObject::pushButton : &PlayerObject::releaseButton;

    if (pl->m_levelSettings->m_twoPlayerMode && pl->m_gameState.m_isDualMode) {
        // в режиме 2 игроков клик управляет только одним из них
        PlayerObject* target = player2 ? pl->m_player2 : pl->m_player1;
        (target->*action)(PlayerButton::Jump);
    } else {
        (pl->m_player1->*action)(PlayerButton::Jump);
        if (pl->m_gameState.m_isDualMode) (pl->m_player2->*action)(PlayerButton::Jump);
    }

    // для триггеров касания
    pl->m_effectManager->playerButton(e.down, !player2);

    if (e.down) {
        pl->m_clicks++;
        pl->m_jumping = true;
    }
}

Analyzer& Analyzer::get() {
    static Analyzer instance;
    return instance;
}

Result<> Analyzer::load(std::filesystem::path const& macroPath) {
    if (m_phase != Phase::Idle) return Ok();
    if (!m_events.empty() && macroPath == m_macroPath) return Ok();

    auto r = loadMacro(macroPath);
    if (r.isErr()) return Err(r.unwrapErr());

    m_events = std::move(r).unwrap();
    m_macroPath = macroPath;
    m_results.clear();

    std::vector<ClickInfo> cached;
    if (loadResults(macroPath, cached)) m_results = std::move(cached);
    return Ok();
}

Result<> Analyzer::start(PlayLayer* pl, std::filesystem::path const& macroPath) {
    if (m_phase != Phase::Idle) return Err("Analysis is already running");
    if (!pl) return Err("No level is running");

    m_events.clear();
    auto loaded = load(macroPath);
    if (loaded.isErr()) return Err(loaded.unwrapErr());

    m_maxShift = static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-shift"));
    m_lookahead = static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead"));
    m_lookahead = std::max(m_lookahead, m_maxShift + 8);

    m_pressIdx.clear();
    for (size_t i = 0; i < m_events.size(); ++i) {
        if (m_events[i].down) m_pressIdx.push_back(i);
    }
    if (m_pressIdx.empty()) return Err("Macro has no clicks");

    m_results.assign(m_pressIdx.size(), ClickInfo{});
    for (size_t i = 0; i < m_pressIdx.size(); ++i) {
        m_results[i].frame = m_events[m_pressIdx[i]].frame;
        m_results[i].p2 = m_events[m_pressIdx[i]].p2;
    }

    m_pl = pl;
    m_k = 0;
    m_baseIdx = 0;
    m_hold = {false, false};
    m_cp = nullptr;
    m_cpFrame = 0;
    m_died = m_ended = m_fatal = m_failed = false;
    m_error.clear();

    uint64_t first = m_results[0].frame;
    m_startFrame = first > static_cast<uint64_t>(m_maxShift) ? first - m_maxShift : 0;

    pl->resetLevelFromStart();

    m_phase = Phase::Advance;
    m_status = "Analyzing...";
    return Ok();
}

float Analyzer::progress() const {
    if (m_pressIdx.empty()) return 0.f;
    return std::clamp(static_cast<float>(m_k) / static_cast<float>(m_pressIdx.size()), 0.f, 1.f);
}

void Analyzer::cancel() {
    if (m_phase == Phase::Idle) return;
    finish(false, "Analysis cancelled");
}

void Analyzer::abandon() {
    if (m_cp) {
        m_cp->release();
        m_cp = nullptr;
    }
    m_simulating = false;
    if (m_phase != Phase::Idle) {
        m_failed = true;
        m_status = "Level closed, analysis aborted";
        m_results.clear();
    }
    m_phase = Phase::Idle;
    m_pl = nullptr;
}

bool Analyzer::tick(double budget) {
    if (m_phase == Phase::Idle) return true;

    if (PlayLayer::get() != m_pl) {
        m_pl = nullptr; // уровень закрыт, трогать его нельзя
        finish(false, "Level closed, analysis aborted");
        return true;
    }

    auto t0 = std::chrono::steady_clock::now();
    auto timeUp = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= budget;
    };

    while (!timeUp()) {
        bool ok = true;
        if (m_phase == Phase::Advance) ok = advanceStep();
        else if (m_phase == Phase::Trials) ok = trialStep();
        else if (m_phase == Phase::Done) {
            finish(true, "Done");
            return true;
        }

        if (!ok) {
            finish(false, m_error.empty() ? "Analysis error" : m_error);
            return true;
        }
        if (m_phase == Phase::Idle) return true;
    }

    m_status = fmt::format("Analyzing: {}/{}", m_k, m_pressIdx.size());
    return false;
}

// один шаг физики ровно на 1 кадр (1/240 с)
bool Analyzer::stepOnce() {
    auto before = progressOf(m_pl);
    float warp = std::min(m_pl->m_gameState.m_timeWarp, 1.f);

    for (int attempt = 0; attempt < 4; ++attempt) {
        m_pl->m_extraDelta = 0.0;
        m_pl->update(warp / 240.f * 1.001f);

        auto after = progressOf(m_pl);
        if (after == before + 1) return true;
        if (after > before + 1) {
            m_error = "The game advanced more than one tick per step (check TPS)";
            m_fatal = true;
            return false;
        }
        // after == before: шаг не случился, пробуем ещё раз
    }
    m_error = "The game does not advance frames (progress is stuck)";
    m_fatal = true;
    return false;
}

void Analyzer::applyBaseline(MacroEvent const& e) {
    applyClick(m_pl, e);
    m_hold[e.p2 ? 1 : 0] = e.down;
}

void Analyzer::syncHolds(std::array<bool, 2> const& hold) {
    bool dual2P = m_pl->m_levelSettings->m_twoPlayerMode && m_pl->m_gameState.m_isDualMode;
    syncOne(m_pl->m_player1, hold[0]);
    syncOne(m_pl->m_player2, dual2P ? hold[1] : hold[0]);
}

bool Analyzer::advanceStep() {
    uint64_t f = progressOf(m_pl);

    if (f < m_startFrame) {
        m_died = false;
        // базовый прогон: применяем события макроса как есть
        while (m_baseIdx < m_events.size() && m_events[m_baseIdx].frame <= f) {
            applyBaseline(m_events[m_baseIdx]);
            ++m_baseIdx;
        }
        m_simulating = true;
        bool ok = stepOnce();
        m_simulating = false;
        if (!ok) return false;
        if (m_died) {
            m_error = "Player died on the baseline run (wrong macro for this level?)";
            return false;
        }
        return true;
    }

    if (!m_cp || m_cpFrame != f) makeCheckpoint();
    if (!m_cp) {
        m_error = "Failed to create a checkpoint";
        return false;
    }
    beginClick();
    m_phase = Phase::Trials;
    return true;
}

void Analyzer::makeCheckpoint() {
    if (m_cp) {
        m_cp->release();
        m_cp = nullptr;
    }
    m_cp = m_pl->createCheckpoint();
    if (!m_cp) return;
    m_cp->retain();
    m_cpFrame = progressOf(m_pl);
    m_cpEventIdx = m_baseIdx;
    m_holdAtCp = m_hold;
}

void Analyzer::beginClick() {
    m_pressEvIdx = m_pressIdx[m_k];
    auto const& e = m_events[m_pressEvIdx];
    int64_t F = static_cast<int64_t>(e.frame);

    // ближайшие события того же игрока: сдвинутый клик не должен их "перепрыгнуть"
    int64_t prev = -1;
    for (size_t i = m_pressEvIdx; i-- > 0;) {
        if (m_events[i].p2 == e.p2) { prev = static_cast<int64_t>(m_events[i].frame); break; }
    }
    int64_t next = INT64_MAX / 4;
    for (size_t i = m_pressEvIdx + 1; i < m_events.size(); ++i) {
        if (m_events[i].p2 == e.p2) { next = static_cast<int64_t>(m_events[i].frame); break; }
    }

    int64_t availEarly = F - static_cast<int64_t>(m_cpFrame);
    m_maxEarly = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>({availEarly, F - prev - 1, m_maxShift})));
    m_maxLate = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(m_maxShift, next - F - 1)));
    m_stage = 0;
}

void Analyzer::endClick() {
    // возвращаем базовое состояние (начало окна этого клика)
    m_pl->loadFromCheckpoint(m_cp);
    m_baseIdx = m_cpEventIdx;
    m_hold = m_holdAtCp;
    syncHolds(m_hold);

    ++m_k;
    if (m_k >= m_pressIdx.size()) {
        m_phase = Phase::Done;
        return;
    }
    uint64_t next = m_results[m_k].frame;
    m_startFrame = next > static_cast<uint64_t>(m_maxShift) ? next - m_maxShift : 0;
    if (m_startFrame < m_cpFrame) m_startFrame = m_cpFrame;
    m_phase = Phase::Advance;
}

// true = игрок пережил весь горизонт (или дошёл до конца уровня)
bool Analyzer::runTrial(int shift, bool record) {
    m_pl->loadFromCheckpoint(m_cp);
    syncHolds(m_holdAtCp);
    m_died = false;
    m_ended = false;

    uint64_t F = m_events[m_pressEvIdx].frame;
    uint64_t endFrame = F + static_cast<uint64_t>(m_lookahead);

    std::vector<MacroEvent> ev;
    for (size_t i = m_cpEventIdx; i < m_events.size(); ++i) {
        MacroEvent e = m_events[i];
        if (e.frame > endFrame + static_cast<uint64_t>(m_maxShift)) break;
        if (i == m_pressEvIdx) {
            e.frame = static_cast<uint64_t>(static_cast<int64_t>(F) + shift);
        }
        ev.push_back(e);
    }
    std::stable_sort(ev.begin(), ev.end(), [](MacroEvent const& a, MacroEvent const& b) {
        return a.frame < b.frame;
    });

    size_t p = 0;
    bool ok = true;
    m_simulating = true;

    while (progressOf(m_pl) < endFrame) {
        uint64_t f = progressOf(m_pl);

        if (record && f == F) {
            PlayerObject* who = m_events[m_pressEvIdx].p2 ? m_pl->m_player2 : m_pl->m_player1;
            m_recX = who->getPositionX();
            m_recY = who->getPositionY();
        }

        while (p < ev.size() && ev[p].frame <= f) {
            applyClick(m_pl, ev[p]);
            ++p;
        }

        if (!stepOnce()) {
            ok = false;
            break;
        }
        if (m_died) {
            ok = false;
            break;
        }
        if (m_ended) break;
    }

    m_simulating = false;
    return ok;
}

bool Analyzer::trialStep() {
    ClickInfo& c = m_results[m_k];

    if (m_stage == 0) {
        // оригинальный клик должен проходить, иначе макрос не совпадает с уровнем
        bool ok = runTrial(0, true);
        if (m_fatal) return false;
        if (!ok) {
            c.valid = false;
            endClick();
            return true;
        }
        c.valid = true;
        c.x = m_recX;
        c.y = m_recY;
        m_lo = 0;
        m_hi = m_maxLate;
        m_stage = 1;
        return true;
    }

    if (m_stage == 1) { // сдвиг ПОЗЖЕ: ищем максимальный проходящий сдвиг бинарным поиском
        if (m_lo >= m_hi) {
            c.late = m_lo;
            m_lo = 0;
            m_hi = m_maxEarly;
            m_stage = 2;
            return true;
        }
        int mid = (m_lo + m_hi + 1) / 2;
        bool ok = runTrial(mid, false);
        if (m_fatal) return false;
        if (ok) m_lo = mid;
        else m_hi = mid - 1;
        return true;
    }

    // m_stage == 2: сдвиг РАНЬШЕ
    if (m_lo >= m_hi) {
        c.early = m_lo;
        endClick();
        return true;
    }
    int mid = (m_lo + m_hi + 1) / 2;
    bool ok = runTrial(-mid, false);
    if (m_fatal) return false;
    if (ok) m_lo = mid;
    else m_hi = mid - 1;
    return true;
}

void Analyzer::finish(bool ok, std::string const& message) {
    m_simulating = false;
    m_phase = Phase::Idle;
    m_failed = !ok;
    m_status = message;

    if (m_cp) {
        m_cp->release();
        m_cp = nullptr;
    }

    // вернуть уровень в нормальное состояние
    if (m_pl && PlayLayer::get() == m_pl) {
        m_pl->resetLevelFromStart();
    }

    if (ok) {
        saveResults(m_macroPath, m_results);
    } else {
        m_results.clear();
    }
    m_pl = nullptr;
}

} // namespace fw
