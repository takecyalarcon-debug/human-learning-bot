#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <vector>
#include <fstream>
#include <random>
#include <cmath>
#include <algorithm>

using namespace geode::prelude;

struct InputAction {
    int tick;
    bool down;
    int button;
    bool isPlayer1;
};

enum class FailStyle {
    None,
    LateClick,
    EarlyRelease
};

namespace BotState {
    std::vector<InputAction> macro;
    int currentTick = 0;
    int botAttempt = 0;
    float deathTargetPercent = 100.0f;
    bool failTriggered = false;
    FailStyle activeFail = FailStyle::None;
    int failDelayTicks = 0;
    bool isSimulating = false;
    bool isHolding = false;

    std::mt19937 rng(std::random_device{}());

    int randomInt(int min, int max) {
        std::uniform_int_distribution<int> dist(min, max);
        return dist(rng);
    }

    float randomFloat(float min, float max) {
        std::uniform_real_distribution<float> dist(min, max);
        return dist(rng);
    }

    std::filesystem::path getMacroPath(int levelID) {
        return Mod::get()->getSaveDir() / fmt::format("macro_{}.txt", levelID);
    }

    void saveMacro(int levelID) {
        std::ofstream out(getMacroPath(levelID));
        for (const auto& act : macro) {
            out << act.tick << " " << act.down << " " << act.button << " " << act.isPlayer1 << "\n";
        }
    }

    void loadMacro(int levelID) {
        macro.clear();
        std::ifstream in(getMacroPath(levelID));
        InputAction act;
        while (in >> act.tick >> act.down >> act.button >> act.isPlayer1) {
            macro.push_back(act);
        }
    }

    void setupAttempt(int attempt, int targetAttempts) {
        currentTick = 0;
        failTriggered = false;
        activeFail = FailStyle::None;
        failDelayTicks = 0;
        isHolding = false;

        if (attempt >= targetAttempts) {
            deathTargetPercent = 101.0f;
            return;
        }

        float ratio = static_cast<float>(attempt) / static_cast<float>(targetAttempts);
        float baseCurve = 100.0f * std::pow(ratio, 0.65f);
        float noise = randomFloat(-6.0f, 6.0f);

        deathTargetPercent = std::clamp(baseCurve + noise, 4.0f, 96.0f);
    }
}

// ==========================================
// INTERFAZ DEL MENÚ DEL MOD (POPUP)
// ==========================================
class BotMenuPopup : public geode::Popup<> {
protected:
    CCMenuItemToggler* m_botToggler = nullptr;
    CCMenuItemToggler* m_recToggler = nullptr;
    TextInput* m_attemptInput = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;

    void updateStatusLabel() {
        if (!m_statusLabel) return;
        int target = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-attempts"));
        std::string rutaInfo = BotState::macro.empty() ? "Sin ruta" : "Ruta lista";
        m_statusLabel->setString(
            fmt::format("{} | Intento: {} / {}", rutaInfo, BotState::botAttempt, target).c_str()
        );
    }

    void setTargetAttempts(int value) {
        int clamped = std::clamp(value, 1, 99999);
        Mod::get()->setSettingValue<int64_t>("target-attempts", clamped);
        if (m_attemptInput) {
            m_attemptInput->setString(std::to_string(clamped));
        }
        updateStatusLabel();
    }

    bool setup() override {
        this->setTitle("Human Learning Bot");

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(menu);

        // 1. Etiqueta de estado arriba
        m_statusLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_statusLabel->setScale(0.55f);
        m_statusLabel->setPosition({m_size.width / 2.f, 184.f});
        m_mainLayer->addChild(m_statusLabel);
        updateStatusLabel();

        bool botEnabled = Mod::get()->getSettingValue<bool>("bot-enabled");
        bool recordMode = Mod::get()->getSettingValue<bool>("record-mode");

        // 2. Casilla: Activar Bot
        m_botToggler = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(BotMenuPopup::onToggleBot), 0.75f
        );
        m_botToggler->setPosition({65.f, 145.f});
        m_botToggler->toggle(botEnabled);
        menu->addChild(m_botToggler);

        auto botLabel = CCLabelBMFont::create("Activar Bot (Jugar)", "bigFont.fnt");
        botLabel->setAnchorPoint({0.f, 0.5f});
        botLabel->setScale(0.5f);
        botLabel->setPosition({90.f, 145.f});
        m_mainLayer->addChild(botLabel);

        // 3. Casilla: Modo Grabación
        m_recToggler = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(BotMenuPopup::onToggleRecord), 0.75f
        );
        m_recToggler->setPosition({65.f, 110.f});
        m_recToggler->toggle(recordMode);
        menu->addChild(m_recToggler);

        auto recLabel = CCLabelBMFont::create("Grabar Ruta (100%)", "bigFont.fnt");
        recLabel->setAnchorPoint({0.f, 0.5f});
        recLabel->setScale(0.5f);
        recLabel->setPosition({90.f, 110.f});
        m_mainLayer->addChild(recLabel);

        // 4. Selector de Intento Objetivo (N)
        auto targetLabel = CCLabelBMFont::create("Pasar en intento:", "bigFont.fnt");
        targetLabel->setAnchorPoint({0.f, 0.5f});
        targetLabel->setScale(0.45f);
        targetLabel->setPosition({42.f, 70.f});
        m_mainLayer->addChild(targetLabel);

        auto minusSpr = ButtonSprite::create("-", "bigFont.fnt", "GJ_button_04.png", 0.6f);
        minusSpr->setScale(0.65f);
        auto minusBtn = CCMenuItemSpriteExtra::create(
            minusSpr, this, menu_selector(BotMenuPopup::onMinusAttempt)
        );
        minusBtn->setPosition({188.f, 70.f});
        menu->addChild(minusBtn);

        int currentTarget = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-attempts"));
        m_attemptInput = TextInput::create(54.f, "35");
        m_attemptInput->setCommonFilter(CommonFilter::Uint);
        m_attemptInput->setMaxCharCount(5);
        m_attemptInput->setPosition({232.f, 70.f});
        m_attemptInput->setString(std::to_string(currentTarget));
        m_attemptInput->setCallback([this](const std::string& str) {
            if (!str.empty()) {
                if (auto res = geode::utils::numFromString<int>(str)) {
                    int val = std::clamp(res.unwrap(), 1, 99999);
                    Mod::get()->setSettingValue<int64_t>("target-attempts", val);
                    updateStatusLabel();
                }
            }
        });
        m_mainLayer->addChild(m_attemptInput);

        auto plusSpr = ButtonSprite::create("+", "bigFont.fnt", "GJ_button_04.png", 0.6f);
        plusSpr->setScale(0.65f);
        auto plusBtn = CCMenuItemSpriteExtra::create(
            plusSpr, this, menu_selector(BotMenuPopup::onPlusAttempt)
        );
        plusBtn->setPosition({276.f, 70.f});
        menu->addChild(plusBtn);

        // 5. Botón Reiniciar Intentos
        auto resetSpr = ButtonSprite::create("Reiniciar Intentos", "goldFont.fnt", "GJ_button_05.png", 0.7f);
        resetSpr->setScale(0.75f);
        auto resetBtn = CCMenuItemSpriteExtra::create(
            resetSpr, this, menu_selector(BotMenuPopup::onResetAttempts)
        );
        resetBtn->setPosition({m_size.width / 2.f, 28.f});
        menu->addChild(resetBtn);

        return true;
    }

    void onToggleBot(CCObject* sender) {
        bool newState = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        Mod::get()->setSettingValue<bool>("bot-enabled", newState);
        if (newState) {
            Mod::get()->setSettingValue<bool>("record-mode", false);
            m_recToggler->toggle(false);
        }
    }

    void onToggleRecord(CCObject* sender) {
        bool newState = !static_cast<CCMenuItemToggler*>(sender)->isToggled();
        Mod::get()->setSettingValue<bool>("record-mode", newState);
        if (newState) {
            Mod::get()->setSettingValue<bool>("bot-enabled", false);
            m_botToggler->toggle(false);
        }
    }

    void onMinusAttempt(CCObject*) {
        int current = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-attempts"));
        setTargetAttempts(current - 1);
    }

    void onPlusAttempt(CCObject*) {
        int current = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-attempts"));
        setTargetAttempts(current + 1);
    }

    void onResetAttempts(CCObject*) {
        BotState::botAttempt = 0;
        updateStatusLabel();
        Notification::create("Contador de intentos reiniciado a 0", NotificationIcon::Info)->show();
    }

public:
    static BotMenuPopup* create() {
        auto ret = new BotMenuPopup();
        if (ret && ret->initAnchored(325.f, 235.f)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

// ==========================================
// BOTONES PARA ABRIR EL MENÚ EN EL JUEGO
// ==========================================
class $modify(BotPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto btnSpr = ButtonSprite::create("Bot", "bigFont.fnt", "GJ_button_01.png", 0.7f);
        btnSpr->setScale(0.65f);
        auto btn = CCMenuItemSpriteExtra::create(
            btnSpr, this, menu_selector(BotPauseLayer::onOpenBotMenu)
        );

        if (auto* rightMenu = this->getChildByID("right-button-menu")) {
            rightMenu->addChild(btn);
            rightMenu->updateLayout();
        }
    }

    void onOpenBotMenu(CCObject*) {
        BotMenuPopup::create()->show();
    }
};

class $modify(BotMainMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto btnSpr = ButtonSprite::create("Bot", "bigFont.fnt", "GJ_button_01.png", 0.7f);
        btnSpr->setScale(0.65f);
        auto btn = CCMenuItemSpriteExtra::create(
            btnSpr, this, menu_selector(BotMainMenuLayer::onOpenBotMenu)
        );

        if (auto* rightMenu = this->getChildByID("right-side-menu")) {
            rightMenu->addChild(btn);
            rightMenu->updateLayout();
        }

        return true;
    }

    void onOpenBotMenu(CCObject*) {
        BotMenuPopup::create()->show();
    }
};

// ==========================================
// LÓGICA DEL BOT Y GRABACIÓN
// ==========================================
class $modify(BotGameLayer, GJBaseGameLayer) {
    void processCommands(float dt) {
        GJBaseGameLayer::processCommands(dt);

        bool botEnabled = Mod::get()->getSettingValue<bool>("bot-enabled");
        bool recordMode = Mod::get()->getSettingValue<bool>("record-mode");

        if (recordMode) {
            BotState::currentTick++;
            return;
        }

        if (!botEnabled || BotState::macro.empty()) return;

        int targetAttempts = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-attempts"));
        bool isWinningAttempt = (BotState::botAttempt >= targetAttempts);

        if (!isWinningAttempt && !BotState::failTriggered) {
            if (auto* pl = PlayLayer::get()) {
                if (pl->getCurrentPercent() >= BotState::deathTargetPercent) {
                    BotState::failTriggered = true;
                    if (BotState::isHolding && BotState::randomInt(0, 1) == 0) {
                        BotState::activeFail = FailStyle::EarlyRelease;
                        BotState::isSimulating = true;
                        this->handleButton(false, 1, true);
                        BotState::isSimulating = false;
                    } else {
                        BotState::activeFail = FailStyle::LateClick;
                        BotState::failDelayTicks = BotState::randomInt(8, 18);
                    }
                }
            }
        }

        for (const auto& act : BotState::macro) {
            int executionTick = act.tick;

            if (!isWinningAttempt) {
                if (BotState::failTriggered) {
                    if (BotState::activeFail == FailStyle::EarlyRelease) {
                        continue;
                    }
                    if (BotState::activeFail == FailStyle::LateClick) {
                        executionTick += BotState::failDelayTicks;
                    }
                } else if (act.tick > 15) {
                    executionTick += BotState::randomInt(-1, 1);
                }
            }

            if (executionTick == BotState::currentTick) {
                BotState::isSimulating = true;
                this->handleButton(act.down, act.button, act.isPlayer1);
                BotState::isSimulating = false;
                BotState::isHolding = act.down;
            }
        }

        BotState::currentTick++;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        bool recordMode = Mod::get()->getSettingValue<bool>("record-mode");
        bool botEnabled = Mod::get()->getSettingValue<bool>("bot-enabled");

        if (recordMode && !BotState::isSimulating) {
            BotState::macro.push_back({BotState::currentTick, down, button, isPlayer1});
        }

        if (botEnabled && !recordMode && !BotState::isSimulating) {
            return;
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(BotPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        BotState::botAttempt = 0;
        BotState::loadMacro(level->m_levelID);
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void resetLevel() {
        bool recordMode = Mod::get()->getSettingValue<bool>("record-mode");
        if (recordMode) {
            BotState::macro.clear();
            BotState::currentTick = 0;
        }

        PlayLayer::resetLevel();

        if (!recordMode) {
            BotState::botAttempt++;
            int targetAttempts = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-attempts"));
            BotState::setupAttempt(BotState::botAttempt, targetAttempts);
        }
    }

    void levelComplete() {
        bool recordMode = Mod::get()->getSettingValue<bool>("record-mode");
        if (recordMode && !BotState::macro.empty()) {
            BotState::saveMacro(m_level->m_levelID);
            Notification::create("Ruta base guardada para el bot", NotificationIcon::Success)->show();
        }
        PlayLayer::levelComplete();
    }
};