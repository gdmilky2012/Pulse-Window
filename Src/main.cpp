#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/file.hpp>
#include <cmath>
#include <map>
#include <string>

#include "macro_parser.hpp"

using namespace geode::prelude;

class PulseHud;
static PulseHud* g_hud = nullptr;

// Hasil analisis macro yang diimpor (dipakai counter jika pengguna memilih)
namespace pulse {
    struct Imported {
        bool valid = false;
        std::map<int, int> hist;
        int total = 0;
    };
    inline Imported imported;
}

class PulseHud : public CCNode {
public:
    struct Slot {
        bool   down        = false;
        bool   usesRelease = false;  // ship / wave / robot
        double start       = 0.0;
    };

protected:
    Slot           m_slot[2];
    std::map<int, int> m_hist;       // frame -> berapa kali
    int            m_total     = 0;
    int            m_active    = -1;
    int            m_colorKey  = -1;
    float          m_layoutT   = 0.f;
    float          m_circleBase = 1.f;
    CCDrawNode*    m_circle    = nullptr;
    CCLabelBMFont* m_num       = nullptr;
    CCLabelBMFont* m_histLabel = nullptr;

    bool init() override {
        if (!CCNode::init()) return false;

        m_circle = CCDrawNode::create();
        m_circle->setVisible(false);
        this->addChild(m_circle, 1);

        m_num = CCLabelBMFont::create("0", "bigFont.fnt");
        m_num->setVisible(false);
        this->addChild(m_num, 2);

        m_histLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_histLabel->setAnchorPoint({1.f, 1.f});
        m_histLabel->setAlignment(kCCTextAlignmentRight);
        this->addChild(m_histLabel, 1);

        this->layout();
        this->refreshHist();
        this->scheduleUpdate();
        return true;
    }

    void onExit() override {
        if (g_hud == this) g_hud = nullptr;
        CCNode::onExit();
    }

    int framesBetween(double t0, double t1) const {
        double rate = static_cast<double>(Mod::get()->getSettingValue<int64_t>("frame-rate"));
        long f = std::lround((t1 - t0) * rate);
        return f < 1 ? 1 : static_cast<int>(f);
    }

    void layout() {
        auto mod = Mod::get();
        auto win = CCDirector::get()->getWinSize();

        auto pos = mod->getSettingValue<std::string>("circle-position");
        CCPoint c = {win.width / 2.f, win.height * 0.82f};
        if (pos == "center")             c = {win.width / 2.f, win.height / 2.f};
        else if (pos == "bottom-center") c = {win.width / 2.f, win.height * 0.18f};
        else if (pos == "left")          c = {win.width * 0.12f, win.height / 2.f};
        else if (pos == "right")         c = {win.width * 0.88f, win.height / 2.f};

        m_circleBase = static_cast<float>(mod->getSettingValue<double>("circle-scale"));
        m_circle->setPosition(c);
        m_num->setPosition(c);
        m_num->setScale(0.6f * m_circleBase);

        m_histLabel->setPosition({win.width - 8.f, win.height - 8.f});
        m_histLabel->setScale(static_cast<float>(mod->getSettingValue<double>("hist-scale")));
        m_histLabel->setVisible(mod->getSettingValue<bool>("show-histogram"));
    }

    // key: 0 hijau, 1 merah, 2 biru (hanya hitung input)
    void drawCircle(int key) {
        if (key == m_colorKey) return;
        m_colorKey = key;
        ccColor3B col = key == 0 ? ccColor3B{90, 230, 140}
                       : key == 1 ? ccColor3B{245, 90, 90}
                                  : ccColor3B{110, 170, 255};
        m_circle->clear();
        float r = 30.f;
        m_circle->drawDot({0, 0}, r + 4.f, {col.r / 255.f, col.g / 255.f, col.b / 255.f, 1.f});
        m_circle->drawDot({0, 0}, r, {0.04f, 0.05f, 0.09f, 0.92f});
        m_num->setColor(col);
    }

    void showValue(int value, bool pressOnly) {
        int target = static_cast<int>(Mod::get()->getSettingValue<int64_t>("target-frames"));
        this->drawCircle(pressOnly ? 2 : (value <= target ? 0 : 1));
        m_num->setString(std::to_string(value).c_str());
    }

    void refreshHist() {
        int rows = static_cast<int>(Mod::get()->getSettingValue<int64_t>("max-rows"));
        std::string s = fmt::format("Input: {}", m_total);
        int n = 0;
        for (auto const& [frame, count] : m_hist) {
            if (n++ >= rows) break;
            s += fmt::format("\nFrame {}: {}", frame, count);
        }
        m_histLabel->setString(s.c_str());
    }

    void hideCircle(float) {
        m_circle->setVisible(false);
        m_num->setVisible(false);
    }

    void popIn() {
        this->unschedule(schedule_selector(PulseHud::hideCircle));
        m_circle->setVisible(true);
        m_num->setVisible(true);
        m_circle->setScale(m_circleBase * 1.25f);
        m_circle->stopAllActions();
        m_circle->runAction(CCEaseOut::create(CCScaleTo::create(0.09f, m_circleBase), 2.f));
    }

public:
    static PulseHud* create() {
        auto ret = new PulseHud();
        if (ret && ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    void press(int idx, bool usesRelease, double t) {
        m_total++;
        m_slot[idx] = {true, usesRelease, t};
        m_active = idx;

        auto mod = Mod::get();
        if (mod->getSettingValue<bool>("sound")) {
            float vol = static_cast<float>(mod->getSettingValue<double>("volume"));
            FMODAudioEngine::get()->playEffect("click.wav"_spr, 1.f, 1.f, vol);
        }

        this->layout();
        this->popIn();
        if (usesRelease) {
            this->showValue(1, false);
        } else {
            // mode tanpa release: hanya input yang dihitung
            this->showValue(m_total, true);
            this->scheduleOnce(schedule_selector(PulseHud::hideCircle), 0.5f);
        }
        this->refreshHist();
    }

    void release(int idx, double t) {
        auto& s = m_slot[idx];
        if (!s.down) return;
        s.down = false;
        if (!s.usesRelease) return;

        int frames = this->framesBetween(s.start, t);
        m_hist[frames]++;
        this->showValue(frames, false);
        this->refreshHist();
        this->scheduleOnce(schedule_selector(PulseHud::hideCircle), 0.8f);
    }

    // Isi counter dengan hasil analisis macro yang diimpor
    void loadImported(std::map<int, int> const& hist, int total) {
        m_hist = hist;
        m_total = total;
        this->refreshHist();
    }

    void clearHolds() {
        m_slot[0] = {};
        m_slot[1] = {};
        m_active = -1;
    }

    void resetAll() {
        this->clearHolds();
        m_hist.clear();
        m_total = 0;
        this->refreshHist();
    }

    void update(float dt) override {
        m_layoutT += dt;
        if (m_layoutT > 0.5f) {
            m_layoutT = 0.f;
            this->layout();
            this->refreshHist();
        }
        // tampilkan durasi berjalan selama tombol ditahan (ship / wave / robot)
        if (m_active >= 0) {
            auto& s = m_slot[m_active];
            auto pl = PlayLayer::get();
            if (s.down && s.usesRelease && pl) {
                this->showValue(this->framesBetween(s.start, pl->m_gameState.m_levelTime), false);
            }
        }
    }
};

class $modify(PulseBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (button != 1 || !g_hud) return;
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;
        if (!Mod::get()->getSettingValue<bool>("enabled")) return;

        int idx = isPlayer1 ? 0 : 1;
        double t = m_gameState.m_levelTime;
        if (down) {
            PlayerObject* p = isPlayer1 ? pl->m_player1 : pl->m_player2;
            bool usesRelease = p && (p->m_isShip || p->m_isDart || p->m_isRobot);
            g_hud->press(idx, usesRelease, t);
        } else {
            g_hud->release(idx, t);
        }
    }
};

class $modify(PulsePlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        if (auto hud = PulseHud::create()) {
            this->addChild(hud, 1000);
            g_hud = hud;
            if (pulse::imported.valid) hud->loadImported(pulse::imported.hist, pulse::imported.total);
        }
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (!g_hud) return;
        if (Mod::get()->getSettingValue<bool>("reset-on-death")) g_hud->resetAll();
        else g_hud->clearHolds();
    }
};

// ======================= Impor macro =======================
// Hanya membaca macro untuk dianalisis (histogram panjang hold). Tidak memutar ulang macro.

static void showImportResult(pmacro::Result const& res, std::string const& fileName) {
    if (!res.ok) {
        FLAlertLayer::create(
            "Impor Macro",
            fmt::format("Gagal membaca {}:\n{}", fileName, res.error),
            "OK"
        )->show();
        return;
    }

    auto mod = Mod::get();
    bool native = mod->getSettingValue<bool>("import-native");
    int64_t refRate = mod->getSettingValue<int64_t>("frame-rate");
    auto st = pmacro::analyze(res.macro, native ? 0.0 : static_cast<double>(refRate));

    std::string text = fmt::format(
        "Format: {}\nFile: {}\nFPS/TPS macro: {:.0f}\n"
        "Input (tekan): {}\nHold lengkap: {}\nHold terpanjang: {} frame\n"
        "Satuan: {}\n",
        res.macro.format, fileName, res.macro.fps,
        st.presses, st.holds, st.longest,
        native ? std::string("frame asli macro") : fmt::format("frame pada {} FPS", refRate)
    );
    if (res.macro.fpsChanged) {
        text += "Catatan: macro mengubah TPS di tengah jalan; durasi tidak dikonversi.\n";
    }
    text += "Macro tidak menyimpan mode game, jadi semua hold (tekan sampai lepas) dihitung.\n\n";

    int rows = 0;
    for (auto const& [frame, count] : st.hist) {
        if (rows++ >= 10) { text += "..."; break; }
        text += fmt::format("Frame {}: {}\n", frame, count);
    }
    if (st.hist.empty()) text += "(tidak ada hold yang lengkap)";

    auto hist = st.hist;
    int total = st.presses;
    createQuickPopup(
        "Impor Macro", text, "Tutup", "Pakai di counter",
        [hist, total](FLAlertLayer*, bool useIt) {
            if (!useIt) return;
            pulse::imported.valid = true;
            pulse::imported.hist = hist;
            pulse::imported.total = total;
            if (g_hud) g_hud->loadImported(hist, total);
            Notification::create("Counter diisi dari macro", NotificationIcon::Success)->show();
        }
    );
}

static void onFilePicked(std::filesystem::path const& path) {
    std::string name = geode::utils::string::pathToString(path.filename());
    auto bytes = file::readBinary(path);
    if (bytes.isErr()) {
        FLAlertLayer::create("Impor Macro", fmt::format("Tidak bisa membuka file: {}", bytes.unwrapErr()), "OK")->show();
        return;
    }
    auto const& data = bytes.unwrap();
    if (data.size() > 64u * 1024u * 1024u) {
        FLAlertLayer::create("Impor Macro", "File terlalu besar (maksimal 64 MB).", "OK")->show();
        return;
    }
    showImportResult(pmacro::parse(data, name), name);
}

static void startImport() {
    file::FilePickOptions opts;
    file::FilePickOptions::Filter filter;
    filter.description = "Macro";
    filter.files = {"*.gdr2", "*.gdr", "*.slc", "*.mhr", "*.json", "*.zbf", "*.txt", "*.xd"};
    opts.filters.push_back(filter);

    async::spawn(
        file::pick(file::PickMode::OpenFile, std::move(opts)),
        [](file::PickResult result) {
            if (result.isErr()) {
                FLAlertLayer::create("Impor Macro", fmt::format("Gagal memilih file: {}", result.unwrapErr()), "OK")->show();
                return;
            }
            auto picked = std::move(result).unwrap();
            if (picked.has_value()) onFilePicked(*picked);   // kosong = dibatalkan
        }
    );
}

static CCMenuItemSpriteExtra* makeImportButton() {
    auto spr = ButtonSprite::create("Impor Macro", 110, true, "bigFont.fnt", "GJ_button_04.png", 28.f, 0.55f);
    return CCMenuItemExt::createSpriteExtra(spr, [](CCObject*) { startImport(); });
}

class $modify(PulsePauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto menu = CCMenu::create();
        menu->addChild(makeImportButton());
        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({win.width - 75.f, 30.f});
        this->addChild(menu, 10);
    }
};

class $modify(PulseMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        auto menu = CCMenu::create();
        menu->addChild(makeImportButton());
        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({75.f, win.height - 25.f});
        this->addChild(menu, 10);
        return true;
    }
};
