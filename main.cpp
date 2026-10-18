#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

using namespace geode::prelude;

namespace fg {

constexpr int kJump = 1;

struct PlayerState {
    double lastGroundedAt = -1000.0;
    bool wasGrounded = false;
    bool coyoteSpent = true;
    bool releasePending = false;
    double releaseDueAt = 0.0;
};

struct State {
    double now = 0.0;
    bool bypass = false;
    std::array<PlayerState, 2> players{};
};

inline State g;

struct Settings {
    bool enabled = true;
    double latePress = 0.0;   // seconds
    double holdExtend = 0.0;  // seconds
    bool debug = false;
};

inline Settings loadSettings() {
    auto* m = Mod::get();
    Settings s;
    s.enabled = m->getSettingValue<bool>("enabled");
    s.latePress = std::clamp<int64_t>(m->getSettingValue<int64_t>("late-press-ms"), 0, 60) / 1000.0;
    s.holdExtend = std::clamp<int64_t>(m->getSettingValue<int64_t>("hold-extend-ms"), 0, 60) / 1000.0;
    s.debug = m->getSettingValue<bool>("debug-log");
    return s;
}

struct Bypass {
    bool prev;
    Bypass() : prev(g.bypass) { g.bypass = true; }
    ~Bypass() { g.bypass = prev; }
};

inline bool inPlay(GJBaseGameLayer* layer) {
    auto* pl = PlayLayer::get();
    return pl != nullptr && static_cast<GJBaseGameLayer*>(pl) == layer;
}

inline bool isCube(PlayerObject* p) {
    return !(p->m_isShip || p->m_isBird || p->m_isBall || p->m_isDart ||
             p->m_isRobot || p->m_isSpider || p->m_isSwing);
}

inline void track(PlayerObject* p, PlayerState& ps) {
    if (!p) return;
    bool grounded = p->m_isOnGround;
    if (grounded) {
        ps.lastGroundedAt = g.now;
        ps.coyoteSpent = false;
    } else if (ps.wasGrounded) {
        // Left the ground this frame. Upward velocity (relative to gravity) means a jump
        // was taken, so the late-press window is spent. Downward means a walk-off.
        double rel = p->m_isUpsideDown ? -p->m_yVelocity : p->m_yVelocity;
        ps.coyoteSpent = rel > 0.0;
    }
    ps.wasGrounded = grounded;
}

}  // namespace fg

class $modify(FGBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (fg::g.bypass || button != fg::kJump) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto s = fg::loadSettings();
        if (!s.enabled || !fg::inPlay(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto& ps = fg::g.players[isPlayer1 ? 0 : 1];
        PlayerObject* player = isPlayer1 ? m_player1 : m_player2;

        if (!down) {
            if (s.holdExtend <= 0.0) {
                GJBaseGameLayer::handleButton(false, button, isPlayer1);
                return;
            }
            if (!ps.releasePending) {
                ps.releasePending = true;
                ps.releaseDueAt = fg::g.now + s.holdExtend;
                if (s.debug) log::debug("release delayed {:.1f} ms (p{})", s.holdExtend * 1000.0, isPlayer1 ? 1 : 2);
            }
            return;
        }

        // Press: flush any pending release first so ordering stays intact.
        if (ps.releasePending) {
            ps.releasePending = false;
            fg::Bypass guard;
            GJBaseGameLayer::handleButton(false, button, isPlayer1);
        }

        if (player) {
            if (player->m_isOnGround) {
                ps.coyoteSpent = true;
            } else if (s.latePress > 0.0 && !ps.coyoteSpent && fg::isCube(player) &&
                       (fg::g.now - ps.lastGroundedAt) <= s.latePress) {
                player->m_isOnGround = true;
                ps.coyoteSpent = true;
                if (s.debug) {
                    log::debug("late press saved, {:.1f} ms after ground (p{})",
                               (fg::g.now - ps.lastGroundedAt) * 1000.0, isPlayer1 ? 1 : 2);
                }
            }
        }

        GJBaseGameLayer::handleButton(true, button, isPlayer1);
    }

    void update(float dt) {
        auto s = fg::loadSettings();
        bool playing = fg::inPlay(this);

        if (playing) {
            fg::g.now += dt;
            for (int i = 0; i < 2; ++i) {
                auto& ps = fg::g.players[i];
                if (ps.releasePending && (!s.enabled || fg::g.now >= ps.releaseDueAt)) {
                    ps.releasePending = false;
                    fg::Bypass guard;
                    GJBaseGameLayer::handleButton(false, fg::kJump, i == 0);
                }
            }
        }

        GJBaseGameLayer::update(dt);

        if (playing && s.enabled) {
            fg::track(m_player1, fg::g.players[0]);
            fg::track(m_player2, fg::g.players[1]);
        }
    }
};

class $modify(FGPlayLayer, PlayLayer) {
    void resetLevel() {
        fg::g = fg::State{};
        {
            fg::Bypass guard;
            PlayLayer::resetLevel();
        }
        fg::g = fg::State{};
    }

    void onQuit() {
        fg::g = fg::State{};
        {
            fg::Bypass guard;
            PlayLayer::onQuit();
        }
        fg::g = fg::State{};
    }
};

$on_mod(Loaded) {
    auto s = fg::loadSettings();
    log::info("Frame Forgiveness loaded: enabled={} late={:.0f}ms hold={:.0f}ms",
              s.enabled, s.latePress * 1000.0, s.holdExtend * 1000.0);
}