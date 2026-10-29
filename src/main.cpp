#include "Analyzer.hpp"
#include "Common.hpp"

#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <Geode/ui/Notification.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
//  Состояние Showcase
// ---------------------------------------------------------------------------
namespace {

struct ShowcaseState {
    bool active = false;     // идёт Showcase: макрос проигрывается, маркеры показываются
    size_t nextEvent = 0;    // следующее событие макроса
    size_t pressOrdinal = 0; // номер нажатия (индекс в результатах анализа)
    std::vector<Ref<CCNode>> markers;
};

ShowcaseState g_show;

bool fileExists(std::string const& name) {
    auto full = std::string(CCFileUtils::sharedFileUtils()->fullPathForFilename(name.c_str(), false));
    std::error_code ec;
    return std::filesystem::exists(full, ec);
}

// ----- ЗАГЛУШКИ: сюда подставь свои картинки и звуки ------------------------
//  Картинка маркера:  resources/marker.png   (если файла нет, рисуется стандартный кружок игры)
//  Звук клика:        resources/click.ogg    (если файла нет, звука нет)
// ---------------------------------------------------------------------------
CCSprite* makeMarkerSprite() {
    std::string custom = "marker.png"_spr;
    if (fileExists(custom)) {
        if (auto s = CCSprite::create(custom.c_str())) return s;
    }
    if (auto s = CCSprite::createWithSpriteFrameName("d_circle_01_001.png")) return s;
    return CCSprite::createWithSpriteFrameName("GJ_button_01.png");
}

void playClickSound() {
    std::string custom = "click.ogg"_spr;
    if (fileExists(custom)) {
        FMODAudioEngine::sharedEngine()->playEffect(custom);
    }
}

void clearMarkers() {
    for (auto& m : g_show.markers) {
        if (m) m->removeFromParent();
    }
    g_show.markers.clear();
}

void toast(std::string const& text, NotificationIcon icon = NotificationIcon::Info) {
    Notification::create(text, icon, 2.5f)->show();
}

void spawnMarker(PlayLayer* pl, fw::ClickInfo const& c) {
    auto root = CCNode::create();
    root->setPosition({c.x, c.y});
    root->setZOrder(1000);

    if (auto circle = makeMarkerSprite()) {
        circle->setScale(0.9f);
        circle->setOpacity(190);
        root->addChild(circle);
    }

    auto label = CCLabelBMFont::create(std::to_string(c.window()).c_str(), "bigFont.fnt");
    label->setScale(0.55f);
    root->addChild(label);

    root->setScale(0.f);
    root->runAction(CCEaseBackOut::create(CCScaleTo::create(0.15f, 1.f)));

    pl->m_objectLayer->addChild(root);
    g_show.markers.push_back(root);

    playClickSound();
}

void showcaseTick(PlayLayer* pl) {
    auto& an = fw::Analyzer::get();
    auto const& events = an.events();
    auto const& results = an.results();
    uint64_t frame = pl->m_gameState.m_currentProgress;

    while (g_show.nextEvent < events.size() && events[g_show.nextEvent].frame <= frame) {
        auto const& e = events[g_show.nextEvent++];
        fw::applyClick(pl, e);

        if (e.down) {
            if (g_show.pressOrdinal < results.size()) {
                auto const& info = results[g_show.pressOrdinal];
                if (info.valid && info.frame == e.frame) spawnMarker(pl, info);
            }
            ++g_show.pressOrdinal;
        }
    }
}

// ---------------------------------------------------------------------------
//  Счётчики в левом верхнем углу
// ---------------------------------------------------------------------------
void refreshCounters(PlayLayer* pl) {
    if (!pl || !pl->m_uiLayer) return;

    if (auto old = pl->m_uiLayer->getChildByID("fw-counters")) old->removeFromParent();

    auto& an = fw::Analyzer::get();
    if (!an.hasResults() || !Mod::get()->getSettingValue<bool>("show-counters")) return;

    std::array<int, fw::kBuckets> counts{};
    int invalid = 0;
    for (auto const& c : an.results()) {
        if (!c.valid) ++invalid;
        else ++counts[fw::bucketIndex(c.window())];
    }

    std::string text;
    for (int i = 0; i < fw::kBuckets; ++i) {
        text += fmt::format("{}: {}\n", fw::bucketName(i), counts[i]);
    }
    if (invalid > 0) text += fmt::format("?: {}\n", invalid);

    auto win = CCDirector::sharedDirector()->getWinSize();
    auto label = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
    label->setID("fw-counters");
    label->setAnchorPoint({0.f, 1.f});
    label->setPosition({6.f, win.height - 6.f});
    label->setScale(0.7f);
    label->setOpacity(210);
    pl->m_uiLayer->addChild(label, 100);
}

} // namespace

// ---------------------------------------------------------------------------
//  PlayLayer
// ---------------------------------------------------------------------------
class $modify(FWPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        // тихо пробуем загрузить макрос и кэш анализа, чтобы показать счётчики
        auto loaded = fw::Analyzer::get().load(fw::configuredMacroPath());
        (void)loaded;
        refreshCounters(this);

        log::info("PlayLayer hook fired, F6 = Analyze, F7 = Showcase");
        toast("Frame Windows: F6 = Analyze, F7 = Showcase");
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        auto& an = fw::Analyzer::get();
        if (an.running()) return;

        clearMarkers();
        if (g_show.active) {
            g_show.nextEvent = 0;
            g_show.pressOrdinal = 0;
            showcaseTick(this); // события нулевого кадра
        }
        refreshCounters(this);
    }

    void onExit() {
        g_show.active = false;
        clearMarkers();
        fw::Analyzer::get().abandon();
        PlayLayer::onExit();
    }

    // во время анализа смерть не должна запускать анимацию/сброс, только фиксируем её
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& an = fw::Analyzer::get();
        if (an.simulating()) {
            an.markDead();
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        auto& an = fw::Analyzer::get();
        if (an.simulating()) {
            an.markEnded();
            return;
        }
        PlayLayer::levelComplete();
    }
};

// ---------------------------------------------------------------------------
//  Проигрывание макроса в Showcase (после каждого шага физики)
// ---------------------------------------------------------------------------
class $modify(FWBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (!g_show.active) return;
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;
        if (fw::Analyzer::get().running()) return;

        showcaseTick(pl);
    }
};

// ---------------------------------------------------------------------------
//  Запуск анализа / Showcase: не зависит от меню паузы
// ---------------------------------------------------------------------------
namespace {

PauseLayer* findPauseLayer() {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    return scene ? scene->getChildByType<PauseLayer>(0) : nullptr;
}

void setPauseMenusEnabled(bool enabled) {
    auto pause = findPauseLayer();
    if (!pause) return;
    for (auto child : CCArrayExt<CCNode*>(pause->getChildren())) {
        if (child->getID() == "fw-controller") continue;
        if (auto menu = typeinfo_cast<CCMenu*>(child)) menu->setEnabled(enabled);
    }
}

} // namespace

// узел на сцене: каждый кадр двигает анализ и показывает прогресс
class FWRunner : public CCNode {
public:
    static FWRunner* create() {
        auto ret = new FWRunner();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init() {
        if (!CCNode::init()) return false;
        this->setID("fw-runner");

        auto win = CCDirector::sharedDirector()->getWinSize();
        m_label = CCLabelBMFont::create("Analyzing...", "bigFont.fnt");
        m_label->setScale(0.6f);
        m_label->setPosition({win.width / 2.f, win.height - 22.f});
        this->addChild(m_label);

        this->scheduleUpdate();
        return true;
    }

    void update(float) override {
        auto& an = fw::Analyzer::get();
        bool done = an.tick(0.012);

        if (an.running()) {
            m_label->setString(fmt::format("{} ({:.0f}%)", an.status(), an.progress() * 100.f).c_str());
        }
        if (!done) return;

        setPauseMenusEnabled(true);
        if (an.lastRunFailed()) {
            toast(an.status(), NotificationIcon::Error);
        } else {
            refreshCounters(PlayLayer::get());
            toast("Analysis finished!", NotificationIcon::Success);
        }
        this->removeFromParent();
    }

private:
    CCLabelBMFont* m_label = nullptr;
};

static void fwToggleAnalysis() {
    auto& an = fw::Analyzer::get();

    if (an.running()) {
        an.cancel();
        toast("Analysis cancelled");
        return;
    }

    auto pl = PlayLayer::get();
    if (!pl) {
        toast("Open a level first", NotificationIcon::Warning);
        return;
    }

    auto scene = CCDirector::sharedDirector()->getRunningScene();
    if (!scene) return;

    // на время анализа уровень должен стоять на паузе
    if (!findPauseLayer()) pl->pauseGame(false);

    g_show.active = false;
    clearMarkers();

    auto res = an.start(pl, fw::configuredMacroPath());
    if (res.isErr()) {
        toast(res.unwrapErr(), NotificationIcon::Error);
        return;
    }

    setPauseMenusEnabled(false);
    if (auto runner = FWRunner::create()) scene->addChild(runner, 9999);
    toast("Analysis started (F6 again to cancel)");
}

static void fwStartShowcase() {
    auto& an = fw::Analyzer::get();
    if (an.running()) {
        toast("Wait for the analysis to finish", NotificationIcon::Warning);
        return;
    }

    auto pl = PlayLayer::get();
    if (!pl) {
        toast("Open a level first", NotificationIcon::Warning);
        return;
    }

    auto loaded = an.load(fw::configuredMacroPath());
    if (loaded.isErr()) {
        toast(loaded.unwrapErr(), NotificationIcon::Error);
        return;
    }
    if (!an.hasResults()) {
        toast("No results yet. Press F6 (Analyze) first", NotificationIcon::Warning);
        return;
    }

    clearMarkers();
    g_show.active = true;
    g_show.nextEvent = 0;
    g_show.pressOrdinal = 0;

    // перезапуск с начала; макрос начнёт играть сам
    if (auto pause = findPauseLayer()) pause->onRestart(nullptr);
    else pl->resetLevelFromStart();

    toast("Showcase started");
}

// ---------------------------------------------------------------------------
//  Горячие клавиши (работают везде, меню паузы не нужно)
// ---------------------------------------------------------------------------
class $modify(FWKeys, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        if (down && !repeat) {
            if (key == enumKeyCodes::KEY_F6) {
                log::info("F6 pressed");
                fwToggleAnalysis();
            } else if (key == enumKeyCodes::KEY_F7) {
                log::info("F7 pressed");
                fwStartShowcase();
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// ---------------------------------------------------------------------------
//  Необязательные кнопки в меню паузы (если хуки паузы сработают)
// ---------------------------------------------------------------------------
class FWController : public CCNode {
public:
    static FWController* create() {
        auto ret = new FWController();
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init() {
        if (!CCNode::init()) return false;
        this->setID("fw-controller");

        auto win = CCDirector::sharedDirector()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        this->addChild(menu);

        auto analyzeSpr = ButtonSprite::create("Analyze", 110, true, "bigFont.fnt", "GJ_button_01.png", 40.f, 0.9f);
        auto analyzeBtn = CCMenuItemSpriteExtra::create(analyzeSpr, this, menu_selector(FWController::onAnalyze));
        analyzeBtn->setPosition({75.f, win.height / 2.f + 30.f});
        menu->addChild(analyzeBtn);

        auto showSpr = ButtonSprite::create("Showcase", 110, true, "bigFont.fnt", "GJ_button_02.png", 40.f, 0.9f);
        auto showBtn = CCMenuItemSpriteExtra::create(showSpr, this, menu_selector(FWController::onShowcase));
        showBtn->setPosition({win.width - 75.f, win.height / 2.f + 30.f});
        menu->addChild(showBtn);
        return true;
    }

    void onAnalyze(CCObject*) { fwToggleAnalysis(); }
    void onShowcase(CCObject*) { fwStartShowcase(); }
};

static void ensureFWUI(PauseLayer* pause) {
    if (!pause || pause->getChildByID("fw-controller")) return;
    if (auto ctrl = FWController::create()) {
        pause->addChild(ctrl, 1000);
        log::info("pause menu UI added");
    }
}

class $modify(FWPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        log::info("PauseLayer::customSetup hook fired");
        ensureFWUI(this);
    }

    void keyBackClicked() {
        // пока идёт анализ, ESC не должен закрывать паузу
        if (fw::Analyzer::get().running()) return;
        PauseLayer::keyBackClicked();
    }
};

class $modify(FWPauseOpenHook, PlayLayer) {
    void pauseGame(bool unfocused) {
        PlayLayer::pauseGame(unfocused);
        log::info("PlayLayer::pauseGame hook fired");
        if (auto pause = findPauseLayer()) ensureFWUI(pause);
    }
};
