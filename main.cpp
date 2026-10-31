#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <deque>
#include <random>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Estado y utilidades
// ---------------------------------------------------------------------------
namespace adi {
    // Máximo de pasos de física que un input puede quedar retenido.
    constexpr int MAX_PENDING_STEPS = 30;

    // Escribe líneas [ADI] en el log de Geode (útil para depurar).
    constexpr bool DEBUG_LOG = true;

    struct Snap {
        PlayerCheckpoint* p1 = nullptr;
        PlayerCheckpoint* p2 = nullptr;
    };

    struct State {
        bool applied[2] = {false, false};  // lo que el juego cree que haces
        bool pending[2] = {false, false};  // primer input de la cola retenido
        int waited[2] = {0, 0};            // pasos que lleva retenido
        std::deque<bool> queue[2];         // clicks (true) y releases (false) por decidir
        float dt = 1.f / 240.f;            // delta de un paso de física
    };

    inline State st;
    inline bool simulating = false;  // true mientras se simulan pasos hacia adelante
    inline bool simDied = false;     // ¿hubo muerte dentro de la simulación?
    inline bool bypass = false;      // true cuando el mod mismo manda un botón al juego

    inline bool enabled() { return Mod::get()->getSettingValue<bool>("enabled"); }
    inline int lookahead() { return (int)Mod::get()->getSettingValue<int64_t>("lookahead"); }

    // Probabilidad de salvarte según el modo
    inline float chance() {
        auto m = Mod::get()->getSettingValue<std::string>("mode");
        if (m == "Hyper Falso") return 1.00f;  // 100%
        if (m == "Semi Humano") return 0.50f;  // 50%
        return 0.10f;                          // Humano: 10%
    }

    // Se llama UNA sola vez por input mortal: el resultado queda decidido ahí.
    inline bool roll(float p) {
        static std::mt19937 rng{std::random_device{}()};
        return std::uniform_real_distribution<float>(0.f, 1.f)(rng) < p;
    }
}

$on_mod(Loaded) {
    log::info("[ADI] mod cargado (v3: cola de inputs)");
}

// ---------------------------------------------------------------------------
// PlayLayer: muerte "simulada" y reset
// ---------------------------------------------------------------------------
class $modify(ADIPlayLayer, PlayLayer) {

    void resetLevel() {
        adi::st = adi::State{};
        PlayLayer::resetLevel();
    }

    // Durante la simulación la muerte NO se ejecuta, solo se anota.
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (adi::simulating) {
            adi::simDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, obj);
    }
};

// ---------------------------------------------------------------------------
// GJBaseGameLayer: filtro de input
// ---------------------------------------------------------------------------
class $modify(ADIGameLayer, GJBaseGameLayer) {

    // Manda un botón al juego "de verdad" (sin pasar por el filtro del mod).
    void realButton(bool down, bool isP1) {
        adi::bypass = true;
        GJBaseGameLayer::handleButton(down, 1, isP1);
        adi::bypass = false;
    }

    // Aplica el input sin comprobar nada.
    void forceButton(int i, bool down) {
        realButton(down, i == 0);
        adi::st.applied[i] = down;
        adi::st.pending[i] = false;
        if (adi::DEBUG_LOG) log::info("[ADI] jugador {} -> {} (forzado)", i + 1, down ? "CLICK" : "RELEASE");
    }

    adi::Snap takeSnap() {
        adi::Snap s;
        s.p1 = PlayerCheckpoint::create();
        s.p2 = PlayerCheckpoint::create();
        m_player1->saveToCheckpoint(s.p1);
        m_player2->saveToCheckpoint(s.p2);
        return s;
    }

    void restoreSnap(adi::Snap const& s) {
        m_player1->loadFromCheckpoint(s.p1);
        m_player2->loadFromCheckpoint(s.p2);
    }

    // Avanza `steps` pasos de física. Devuelve true si habría muerte.
    // (Quien llama es responsable de restaurar el estado después.)
    bool runSteps(int steps) {
        adi::simulating = true;
        adi::simDied = false;

        for (int k = 0; k < steps && !adi::simDied; ++k) {
            m_player1->update(adi::st.dt);
            this->checkCollisions(m_player1, adi::st.dt, false);
            if (m_gameState.m_isDualMode) {
                m_player2->update(adi::st.dt);
                this->checkCollisions(m_player2, adi::st.dt, true);
            }
        }

        adi::simulating = false;
        return adi::simDied;
    }

    // ¿Moriría si NO cambio nada y sigo como estoy?
    bool stayingPutDies(int steps) {
        auto s = takeSnap();
        bool died = runSteps(steps);
        restoreSnap(s);
        return died;
    }

    // Aplica el input de verdad y mira hacia adelante.
    // - Si es seguro: se queda aplicado (igual que en el juego normal) y devuelve true.
    // - Si te mataría: lo deshace por completo y devuelve false.
    bool tryCommit(int i, bool want, int steps) {
        bool isP1 = (i == 0);
        auto before = takeSnap();

        realButton(want, isP1);  // el input se aplica UNA vez, como en el juego normal

        auto after = takeSnap();
        bool died = runSteps(steps);
        restoreSnap(after);      // solo se rebobinan los pasos simulados

        if (!died) {
            adi::st.applied[i] = want;
            if (adi::DEBUG_LOG) log::info("[ADI] jugador {} -> {} (seguro)", i + 1, want ? "CLICK" : "RELEASE");
            return true;
        }

        // Deshacer: volver al estado de antes y devolver el botón a como estaba
        restoreSnap(before);
        realButton(adi::st.applied[i], isP1);
        if (adi::DEBUG_LOG) log::info("[ADI] jugador {}: {} te mataría", i + 1, want ? "CLICK" : "RELEASE");
        return false;
    }

    // Decide, al inicio de un paso, qué hacer con los inputs en cola del jugador i.
    void drainQueue(int i, bool active, int steps) {
        auto& q = adi::st.queue[i];

        while (!q.empty()) {
            bool want = q.front();

            // Mod apagado, o el juego ya cree que estás así: pasa directo.
            if (!active || want == adi::st.applied[i]) {
                q.pop_front();
                forceButton(i, want);
                continue;
            }

            // 1) ¿Es seguro aplicarlo ya? -> se aplica en este mismo paso.
            if (tryCommit(i, want, steps)) {
                q.pop_front();
                adi::st.pending[i] = false;
                continue;
            }

            // 2) Te mataría y todavía no se ha decidido nada.
            if (!adi::st.pending[i]) {
                // Sin salida (quedarte quieto también mata) o el dado dice que no: pasa tal cual.
                if (stayingPutDies(steps) || !adi::roll(adi::chance())) {
                    q.pop_front();
                    forceButton(i, want);
                    continue;
                }
                // El dado dijo que sí: se retiene hasta el primer paso seguro.
                adi::st.pending[i] = true;
                adi::st.waited[i] = 0;
                if (adi::DEBUG_LOG) log::info("[ADI] jugador {}: input retenido", i + 1);
                break;
            }

            // 3) Ya estaba retenido: ¿se acabó la espera o ya no hay salida?
            if (++adi::st.waited[i] > adi::MAX_PENDING_STEPS || stayingPutDies(steps)) {
                q.pop_front();
                forceButton(i, want);
                continue;
            }
            break;  // sigue esperando
        }
    }

    // El juego avisa de un click/release: el mod lo guarda en la cola y lo decide
    // al inicio del siguiente paso, FUERA del procesamiento de inputs del juego.
    void handleButton(bool down, int button, bool isP1) {
        bool passthrough = !adi::enabled()
            || button != 1                 // izquierda/derecha del platformer pasan directo
            || adi::bypass
            || adi::simulating
            || !PlayLayer::get();          // editor, etc.

        if (passthrough) {
            if (button == 1 && !adi::bypass && !adi::simulating) {
                adi::st.applied[isP1 ? 0 : 1] = down;
            }
            return GJBaseGameLayer::handleButton(down, button, isP1);
        }

        adi::st.queue[isP1 ? 0 : 1].push_back(down);
        if (adi::DEBUG_LOG) log::info("[ADI] jugador {}: {} recibido", isP1 ? 1 : 2, down ? "CLICK" : "RELEASE");
    }

    // En esta versión de GD la función recibe 3 argumentos.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        adi::st.dt = dt;

        if (PlayLayer::get() && !adi::simulating) {
            bool active = adi::enabled();
            int steps = adi::lookahead();
            for (int i = 0; i < 2; ++i) {
                drainQueue(i, active, steps);
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};
