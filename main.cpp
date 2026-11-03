#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <random>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Input Buffer
//
// Si das un click un poco ANTES de aterrizar (cubo, bola, robot, araña), el mod
// lo guarda y lo dispara en el primer paso de física en que el jugador toca el
// suelo (o un aro). En el resto de los casos el click pasa directo, sin retraso.
// ---------------------------------------------------------------------------
namespace ib {
    constexpr bool DEBUG_LOG = true;

    // Pasos que dura el click cuando ya habías soltado el dedo antes de aterrizar
    constexpr int RELEASE_DELAY_STEPS = 2;

    struct State {
        bool buffered[2] = {false, false};    // click retenido esperando aterrizar
        bool fingerDown[2] = {false, false};  // tu dedo sigue en la pantalla
        bool held[2] = {false, false};        // lo que el juego cree que haces
        int waited[2] = {0, 0};               // pasos que lleva retenido
        int releaseIn[2] = {0, 0};            // pasos para soltar un click ya disparado
    };

    inline State st;
    inline bool bypass = false;  // true cuando el mod mismo manda un botón al juego

    inline bool enabled() { return Mod::get()->getSettingValue<bool>("enabled"); }

    struct Mode {
        float chance;    // probabilidad de que el buffer actúe en cada click temprano
        float windowMs;  // cuánto antes de aterrizar puedes clickear
    };

    inline Mode mode() {
        auto m = Mod::get()->getSettingValue<std::string>("mode");
        if (m == "Hyper Falso") return {1.00f, 200.f};  // 100%
        if (m == "Semi Humano") return {0.50f, 120.f};  // 50%
        return {0.10f, 60.f};                           // Humano: 10%
    }

    // El dado se tira UNA sola vez por click.
    inline bool roll(float p) {
        static std::mt19937 rng{std::random_device{}()};
        return std::uniform_real_distribution<float>(0.f, 1.f)(rng) < p;
    }

    // Solo modos donde el click depende de tocar el suelo
    inline bool bufferableMode(PlayerObject* p) {
        return p && !p->m_isShip && !p->m_isBird && !p->m_isDart
                 && !p->m_isSwing && !p->m_isPlatformer;
    }

    // ¿Está tocando un aro? (esos clicks NO se retienen)
    inline bool touchingRing(PlayerObject* p) {
        return p && p->m_touchingRings && p->m_touchingRings->count() > 0;
    }
}

$on_mod(Loaded) {
    log::info("[IB] mod cargado (v4: input buffer simple, sin simulacion)");
}

// ---------------------------------------------------------------------------
// PlayLayer: limpiar el estado al reiniciar el intento
// ---------------------------------------------------------------------------
class $modify(IBPlayLayer, PlayLayer) {
    void resetLevel() {
        ib::st = ib::State{};
        PlayLayer::resetLevel();
    }
};

// ---------------------------------------------------------------------------
// GJBaseGameLayer: el buffer
// ---------------------------------------------------------------------------
class $modify(IBGameLayer, GJBaseGameLayer) {

    PlayerObject* playerFor(int i) {
        return i == 0 ? m_player1 : m_player2;
    }

    // Manda un botón al juego "de verdad" (sin pasar por el filtro del mod).
    void sendButton(int i, bool down) {
        ib::bypass = true;
        GJBaseGameLayer::handleButton(down, 1, i == 0);
        ib::bypass = false;
        ib::st.held[i] = down;
    }

    void handleButton(bool down, int button, bool isP1) {
        int i = isP1 ? 0 : 1;

        bool passthrough = !ib::enabled()
            || button != 1            // izquierda/derecha del platformer pasan directo
            || ib::bypass
            || !PlayLayer::get();     // editor, etc.

        if (passthrough) {
            if (button == 1 && !ib::bypass) {
                ib::st.held[i] = down;
            }
            return GJBaseGameLayer::handleButton(down, button, isP1);
        }

        auto p = playerFor(i);

        if (down) {
            ib::st.fingerDown[i] = true;
            ib::st.releaseIn[i] = 0;  // un click nuevo cancela cualquier soltado programado

            bool candidate = !ib::st.held[i]
                && !ib::st.buffered[i]
                && ib::bufferableMode(p)
                && !p->m_isOnGround      // en el suelo el click pasa directo
                && !ib::touchingRing(p); // el click de un aro pasa directo

            if (candidate && ib::roll(ib::mode().chance)) {
                ib::st.buffered[i] = true;
                ib::st.waited[i] = 0;
                if (ib::DEBUG_LOG) log::info("[IB] jugador {}: click temprano retenido", i + 1);
                return;  // retenido hasta aterrizar
            }

            ib::st.held[i] = true;
            return GJBaseGameLayer::handleButton(down, button, isP1);
        }

        // release
        ib::st.fingerDown[i] = false;
        if (ib::st.buffered[i]) {
            return;  // el click todavía no se aplicó: el release se resuelve al aterrizar
        }
        ib::st.held[i] = false;
        GJBaseGameLayer::handleButton(down, button, isP1);
    }

    // Antes de cada paso de física: ¿ya aterrizó algún jugador con un click retenido?
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (PlayLayer::get()) {
            bool on = ib::enabled();
            float stepMs = std::max(dt * 1000.f, 0.1f);
            int windowSteps = std::max(1, (int)(ib::mode().windowMs / stepMs));

            for (int i = 0; i < 2; ++i) {
                // Soltar un click ya disparado (cuando soltaste el dedo antes de aterrizar)
                if (ib::st.releaseIn[i] > 0 && --ib::st.releaseIn[i] == 0) {
                    if (!ib::st.fingerDown[i]) sendButton(i, false);
                }

                if (!ib::st.buffered[i]) continue;

                // Si el mod se apagó con un click retenido, soltarlo tal cual
                if (!on) {
                    ib::st.buffered[i] = false;
                    if (ib::st.fingerDown[i]) sendButton(i, true);
                    continue;
                }

                auto p = playerFor(i);
                bool fire = !p
                    || p->m_isOnGround
                    || ib::touchingRing(p)
                    || !ib::bufferableMode(p);

                if (fire) {
                    ib::st.buffered[i] = false;
                    sendButton(i, true);
                    if (!ib::st.fingerDown[i]) ib::st.releaseIn[i] = ib::RELEASE_DELAY_STEPS;
                    if (ib::DEBUG_LOG) log::info("[IB] jugador {}: click disparado al aterrizar", i + 1);
                }
                else if (++ib::st.waited[i] > windowSteps) {
                    // Se acabó la ventana sin aterrizar
                    ib::st.buffered[i] = false;
                    if (ib::st.fingerDown[i]) sendButton(i, true);  // sigues presionando: como en el juego normal
                    if (ib::DEBUG_LOG) log::info("[IB] jugador {}: ventana agotada", i + 1);
                }
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};
