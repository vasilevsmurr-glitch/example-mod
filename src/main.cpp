// =============================================================================
//  GD Bot — мод для Geode (GD 2.2081, Geode 5.x)
//
//  Режимы (горячие клавиши):
//    F6 - LEARN : бот учится методом проб и ошибок. Умирает, запоминает, где
//                 кликать, и с каждой попыткой уходит дальше. Память хранится
//                 в файле для каждого уровня, так что прогресс не теряется.
//    F7 - HITBOX: бот смотрит на хитбоксы шипов и блоков впереди, симулирует
//                 прыжок и прыгает в последний безопасный момент.
//    F8 - OFF   : выключить бота.
//    F9 - стереть память LEARN для текущего уровня.
//
//  Ограничения: HITBOX работает только для куба с обычной гравитацией и не
//  знает про орбы, пэды, порталы и склоны. Используйте мод только офлайн
// =============================================================================

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace geode::prelude;

// -----------------------------------------------------------------------------
//  Общее состояние бота
// -----------------------------------------------------------------------------
enum class BotMode { Off, Learn, Hitbox };

struct Hold { float a, b; };                  // удержание кнопки от x=a до x=b
struct Box  { float x0, y0, x1, y1; };        // прямоугольный хитбокс

namespace bot {
    BotMode mode = BotMode::Off;

    bool  holding = false;                    // что бот сейчас "нажимает"
    int   frame = 0;                          // шаги с начала попытки
    int   releaseFrame = -1;                  // когда отпустить (HITBOX)
    float lastX = 0.f;
    float speed = 0.f;                        // скорость игрока, ед./сек

    // кэш хитбоксов уровня (HITBOX)
    std::vector<Box> hazards, solids;

    // обучение (LEARN)
    std::vector<Hold> best, plan;             // лучший известный и текущий планы
    float  bestX = 0.f;                       // докуда дошёл лучший план
    int    attempts = 0;
    int    failsInRow = 0;
    bool   solved = false;
    bool   deathHandled = false;
    size_t idx = 0;
    std::string key;                          // имя файла памяти для уровня
    std::mt19937 rng{ std::random_device{}() };
}

static float frand(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(bot::rng);
}

// Нажать/отпустить кнопку прыжка (только при изменении состояния)
static void setButton(GJBaseGameLayer* gl, bool down) {
    if (bot::holding == down) return;
    bot::holding = down;
    gl->handleButton(down, 1, true);
}

static const char* modeName(BotMode m) {
    switch (m) {
        case BotMode::Learn:  return "LEARN";
        case BotMode::Hitbox: return "HITBOX";
        default:              return "OFF";
    }
}

// =============================================================================
//  БОТ 1: обучение методом проб и ошибок
//
//  Идея: план — это список участков x, где кнопка зажата. Бот проходит по плану.
//  Когда он умирает в точке X:
//    * если дошёл не хуже лучшего плана — этот план становится "лучшим"
//      (всё, что до точки смерти, теперь закреплено в памяти);
//    * иначе возвращаемся к лучшему плану.
//  Следующая попытка = лучший план + случайная мутация рядом с местом смерти
//  (удалить клик, сдвинуть клик, добавить клик). Чем дольше бот застревает,
//  тем шире окно мутаций.
// =============================================================================
static std::filesystem::path savePath() {
    return Mod::get()->getSaveDir() / (bot::key + ".txt");
}

static void saveLearn() {
    std::ofstream f(savePath());
    if (!f) return;
    f << bot::bestX << ' ' << (bot::solved ? 1 : 0) << ' ' << bot::attempts << '\n';
    for (auto& h : bot::best) f << h.a << ' ' << h.b << '\n';
}

static void loadLearn() {
    bot::best.clear();
    bot::bestX = 0.f;
    bot::attempts = 0;
    bot::failsInRow = 0;
    bot::solved = false;

    std::ifstream f(savePath());
    if (!f) return;
    int solved = 0;
    f >> bot::bestX >> solved >> bot::attempts;
    bot::solved = solved != 0;
    float a, b;
    while (f >> a >> b) bot::best.push_back({ a, b });
}

static void resetLearn() {
    std::error_code ec;
    std::filesystem::remove(savePath(), ec);
    bot::best.clear();
    bot::plan.clear();
    bot::bestX = 0.f;
    bot::attempts = 0;
    bot::failsInRow = 0;
    bot::solved = false;
}

// Сортировка, удаление пустых участков, слияние пересекающихся
static void normalize(std::vector<Hold>& v) {
    v.erase(std::remove_if(v.begin(), v.end(),
        [](const Hold& h) { return h.b - h.a < 1.f; }), v.end());
    std::sort(v.begin(), v.end(), [](const Hold& l, const Hold& r) { return l.a < r.a; });

    std::vector<Hold> out;
    for (auto& h : v) {
        if (!out.empty() && h.a <= out.back().b + 2.f) out.back().b = std::max(out.back().b, h.b);
        else out.push_back(h);
    }
    v = std::move(out);
}

static std::vector<Hold> mutate(const std::vector<Hold>& base, float deathX, int fails) {
    auto p = base;

    // окно мутаций растёт вместе с числом неудач подряд
    float W  = std::min(60.f + fails * 10.f, 600.f);
    float lo = std::max(0.f, deathX - W);
    float hi = deathX + 15.f;

    int ops = 1 + (frand(0.f, 1.f) < 0.3f ? 1 : 0);
    for (int i = 0; i < ops; i++) {
        float r = frand(0.f, 1.f);

        if (r < 0.25f && !p.empty()) {
            // 1) стереть клики в случайном отрезке окна
            float a = frand(lo, hi);
            float b = a + frand(10.f, W);
            p.erase(std::remove_if(p.begin(), p.end(),
                [&](const Hold& h) { return h.b > a && h.a < b; }), p.end());
        }
        else if (r < 0.45f && !p.empty()) {
            // 2) сдвинуть один клик из окна
            std::vector<size_t> cand;
            for (size_t k = 0; k < p.size(); k++) if (p[k].b > lo) cand.push_back(k);
            if (!cand.empty()) {
                auto& h = p[cand[std::uniform_int_distribution<size_t>(0, cand.size() - 1)(bot::rng)]];
                float d = frand(-25.f, 25.f);
                h.a += d;
                h.b += d;
            }
        }
        else {
            // 3) добавить новый клик (чаще короткий тап, иногда долгое удержание)
            float a = frand(lo, hi);
            float len = frand(0.f, 1.f) < 0.8f ? frand(4.f, 40.f) : frand(40.f, 220.f);
            p.push_back({ a, a + len });
        }
    }
    normalize(p);
    return p;
}

// Вызывается в начале каждой попытки
static void prepareAttempt() {
    if (bot::solved)            bot::plan = bot::best;       // уровень пройден — просто повторяем
    else if (bot::bestX <= 0.f) bot::plan.clear();           // самая первая попытка
    else                        bot::plan = mutate(bot::best, bot::bestX, bot::failsInRow);
}

// Вызывается в момент смерти
static void onDeath(float x) {
    bot::attempts++;

    if (x >= bot::bestX - 1.f) {
        if (x > bot::bestX + 1.f) bot::failsInRow = 0;
        else                      bot::failsInRow++;

        bot::bestX = std::max(bot::bestX, x);
        bot::best = bot::plan;
        // клики после точки смерти мы не проверяли — не запоминаем
        bot::best.erase(std::remove_if(bot::best.begin(), bot::best.end(),
            [x](const Hold& h) { return h.a > x; }), bot::best.end());
        saveLearn();
    } else {
        bot::failsInRow++;       // хуже лучшего — откатываемся (best не меняется)
    }
}

static void learnTick(GJBaseGameLayer* gl) {
    float x = gl->m_player1->getPositionX();
    while (bot::idx < bot::plan.size() && x >= bot::plan[bot::idx].b) bot::idx++;
    bool want = bot::idx < bot::plan.size() && x >= bot::plan[bot::idx].a;
    setButton(gl, want);
}

// =============================================================================
//  БОТ 2: прыжки по хитбоксам
//
//  На каждом шаге, пока куб стоит на земле, бот симулирует будущее:
//    A) что будет, если не прыгать;
//    B) что будет, если прыгнуть прямо сейчас;
//    C) что будет, если прыгнуть через один шаг.
//  Если A заканчивается смертью, а B выживает и при этом C уже нет — это
//  последний безопасный момент, и бот прыгает.
// =============================================================================
constexpr float kGravity    = 3449.f;   // ед./с^2 (примерно, подгонялось под куб)
constexpr float kJumpSpeed  = 671.f;    // начальная скорость прыжка, ед./с
constexpr float kHalf       = 15.f;     // половина стороны куба (для блоков)
constexpr float kHazardHalf = 6.f;      // малый хитбокс куба для шипов

static Box toBox(const CCRect& r) {
    return { r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY() };
}

static void buildCache(GJBaseGameLayer* gl) {
    bot::hazards.clear();
    bot::solids.clear();

    for (auto obj : CCArrayExt<GameObject*>(gl->m_objects)) {
        if (!obj) continue;
        switch (obj->m_objectType) {
            case GameObjectType::Hazard: bot::hazards.push_back(toBox(obj->getObjectRect())); break;
            case GameObjectType::Solid:  bot::solids.push_back(toBox(obj->getObjectRect()));  break;
            default: break;
        }
    }
    auto byX = [](const Box& l, const Box& r) { return l.x0 < r.x0; };
    std::sort(bot::hazards.begin(), bot::hazards.end(), byX);
    std::sort(bot::solids.begin(),  bot::solids.end(),  byX);
}

static void gather(const std::vector<Box>& src, float lo, float hi, std::vector<const Box*>& out) {
    out.clear();
    auto it = std::lower_bound(src.begin(), src.end(), lo,
        [](const Box& b, float v) { return b.x0 < v; });
    for (; it != src.end() && it->x0 <= hi; ++it) out.push_back(&*it);
}

static bool overlaps(const Box& b, float x0, float y0, float x1, float y1) {
    return b.x0 < x1 && b.x1 > x0 && b.y0 < y1 && b.y1 > y0;
}

struct SimResult { bool alive; float tDeath; };

// Симуляция куба: движение вправо с постоянной скоростью vx, гравитация,
// один прыжок через jumpAfter секунд (если он >= horizon — прыжка нет).
static SimResult simulate(float x, float y, float vx, float jumpAfter,
                          float horizon, float groundTop, float pad)
{
    static thread_local std::vector<const Box*> hz, sl;
    gather(bot::hazards, x - 200.f, x + vx * horizon + 80.f, hz);
    gather(bot::solids,  x - 200.f, x + vx * horizon + 80.f, sl);

    const float dt = 1.f / 120.f;
    const float hh = kHazardHalf + pad;
    float vy = 0.f, t = 0.f;
    bool grounded = true, jumped = false;

    while (t < horizon) {
        if (!jumped && t >= jumpAfter) {
            if (!grounded) return { false, t };      // прыгнуть не из чего
            vy = kJumpSpeed;
            grounded = false;
            jumped = true;
        }

        float prevBottom = y - kHalf;
        x  += vx * dt;
        vy -= kGravity * dt;
        y  += vy * dt;
        grounded = false;

        // пол
        if (y - kHalf <= groundTop) { y = groundTop + kHalf; vy = 0.f; grounded = true; }

        // блоки: сверху — приземляемся, сбоку — смерть
        for (auto s : sl) {
            if (!overlaps(*s, x - kHalf, y - kHalf, x + kHalf, y + kHalf)) continue;
            if (vy <= 0.f && prevBottom >= s->y1 - 6.f) {
                y = s->y1 + kHalf; vy = 0.f; grounded = true;
            } else {
                return { false, t };
            }
        }

        // шипы и прочие опасные объекты
        for (auto h : hz) {
            if (overlaps(*h, x - hh, y - hh, x + hh, y + hh)) return { false, t };
        }
        t += dt;
    }
    return { true, horizon };
}

static void hitboxTick(GJBaseGameLayer* gl, float dt) {
    auto p = gl->m_player1;

    // поддерживается только обычный куб
    if (p->m_isShip || p->m_isBird || p->m_isBall || p->m_isDart ||
        p->m_isRobot || p->m_isSpider || p->m_isSwing || p->m_isUpsideDown) return;

    // короткий тап: отпускаем через пару шагов
    if (bot::holding && bot::frame >= bot::releaseFrame) setButton(gl, false);
    if (bot::holding) return;

    if (!p->m_isOnGround || bot::speed <= 1.f) return;

    auto mod = Mod::get();
    float horizon   = static_cast<float>(mod->getSettingValue<double>("lookahead"));
    float pad       = static_cast<float>(mod->getSettingValue<double>("hazard-pad"));
    float groundTop = static_cast<float>(mod->getSettingValue<double>("ground-top"));

    float x = p->getPositionX();
    float y = p->getPositionY();
    float step = std::max(dt, 1.f / 240.f);

    auto idle  = simulate(x, y, bot::speed, 1e9f, horizon, groundTop, pad);
    if (idle.alive) return;                                     // и так всё хорошо

    auto now   = simulate(x, y, bot::speed, 0.f,  horizon, groundTop, pad);
    auto later = simulate(x, y, bot::speed, step, horizon, groundTop, pad);

    bool lastSafeMoment = now.alive && !later.alive;
    bool desperate      = !now.alive && idle.tDeath < 0.10f;    // всё равно пробуем

    if (lastSafeMoment || desperate) {
        setButton(gl, true);
        bot::releaseFrame = bot::frame + 2;
    }
}

// =============================================================================
//  Хуки
// =============================================================================
class $modify(BotBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt) {
        auto pl = PlayLayer::get();
        if (bot::mode != BotMode::Off && pl &&
            static_cast<GJBaseGameLayer*>(pl) == this &&
            m_player1 && !m_player1->m_isDead)
        {
            float x = m_player1->getPositionX();
            if (dt > 0.f && bot::frame > 0 && x > bot::lastX) bot::speed = (x - bot::lastX) / dt;
            bot::lastX = x;

            if (bot::mode == BotMode::Learn) learnTick(this);
            else                             hitboxTick(this, dt);

            bot::frame++;
        }
        GJBaseGameLayer::processCommands(dt);
    }
};

class $modify(BotPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* hud = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        // ключ памяти: имя уровня + id
        std::string name = m_level->m_levelName;
        for (auto& c : name) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        bot::key = fmt::format("{}_{}", name, m_level->m_levelID.value());

        buildCache(this);
        loadLearn();
        bot::plan = bot::best;
        bot::holding = false;
        bot::frame = 0;
        bot::idx = 0;

        // маленький HUD в левом верхнем углу
        auto hud = CCLabelBMFont::create("", "bigFont.fnt");
        hud->setScale(0.35f);
        hud->setAnchorPoint({ 0.f, 1.f });
        hud->setOpacity(170);
        hud->setPosition({ 8.f, CCDirector::get()->getWinSize().height - 8.f });
        m_uiLayer->addChild(hud, 100);
        m_fields->hud = hud;
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        if (bot::holding) {          // отпускаем кнопку, чтобы не "залипла"
            bot::holding = false;
            this->handleButton(false, 1, true);
        }
        bot::frame = 0;
        bot::idx = 0;
        bot::lastX = 0.f;
        bot::speed = 0.f;
        bot::releaseFrame = -1;
        bot::deathHandled = false;

        if (bot::mode == BotMode::Learn) prepareAttempt();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (bot::mode == BotMode::Learn && !bot::deathHandled &&
            player == m_player1 && !m_player1->m_isDead)
        {
            bot::deathHandled = true;
            onDeath(player->getPositionX());
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (bot::mode == BotMode::Learn && !bot::solved) {
            bot::solved = true;
            bot::bestX = std::max(bot::bestX, m_player1->getPositionX());
            bot::best = bot::plan;
            saveLearn();
        }
        PlayLayer::levelComplete();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto hud = m_fields->hud;
        if (!hud || bot::frame % 6 != 0) return;

        if (bot::mode == BotMode::Off) { hud->setString(""); return; }

        float len = std::max(m_levelLength, 1.f);
        float now = m_player1->getPositionX() / len * 100.f;

        if (bot::mode == BotMode::Learn) {
            hud->setString(fmt::format(
                "LEARN | attempt {} | best {:.0f}% | now {:.0f}%{}",
                bot::attempts, bot::bestX / len * 100.f, now,
                bot::solved ? " | SOLVED" : "").c_str());
        } else {
            hud->setString(fmt::format(
                "HITBOX | speed {:.0f} | hazards {} | now {:.0f}%",
                bot::speed, bot::hazards.size(), now).c_str());
        }
    }
};

class $modify(BotKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat) {
        if (down && !repeat) {
            bool changed = false;

            if (key == KEY_F6)      { bot::mode = BotMode::Learn;  bot::plan = bot::best; changed = true; }
            else if (key == KEY_F7) { bot::mode = BotMode::Hitbox; changed = true; }
            else if (key == KEY_F8) { bot::mode = BotMode::Off;    changed = true; }
            else if (key == KEY_F9) {
                resetLearn();
                Notification::create("Learned data erased", NotificationIcon::Success, 1.f)->show();
            }

            if (changed) {
                if (bot::holding) {   // не оставляем кнопку зажатой при смене режима
                    bot::holding = false;
                    if (auto pl = PlayLayer::get()) pl->handleButton(false, 1, true);
                }
                Notification::create(fmt::format("Bot: {}", modeName(bot::mode)),
                                     NotificationIcon::None, 1.f)->show();
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat);
    }
};
