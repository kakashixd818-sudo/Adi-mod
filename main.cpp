#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <random>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Estado y utilidades
// ---------------------------------------------------------------------------
namespace adi {
    // Máximo de pasos de física que un input puede quedar retenido.
    // Garantiza que NUNCA se quede el botón "pegado" por un falso positivo.
    constexpr int MAX_PENDING_STEPS = 30;

    struct State {
        bool desired[2] = {false, false};  // lo que TÚ haces con la pantalla
        bool applied[2] = {false, false};  // lo que el juego cree que haces
        bool pending[2] = {false, false};  // input retenido esperando un hueco seguro
        int waited[2] = {0, 0};            // pasos que lleva retenido
        float dt = 1.f / 240.f;            // delta de un paso de física
    };

    inline State st;
    inline bool simulating = false;  // true mientras probamos un futuro hipotético
    inline bool simDied = false;     // ¿hubo muerte dentro de la simulación?

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

    // Simula `steps` pasos. Si `changeInput` es true, primero aplica el botón
    // `hold` (como si lo hicieras ahora); si es false, simula seguir como estás.
    // Devuelve true si en algún paso habría muerte. Todo se restaura al final.
    bool simulate(bool changeInput, bool hold, bool isP1, int steps) {
        auto p1 = m_player1;
        auto p2 = m_player2;

        auto snap1 = PlayerCheckpoint::create();
        auto snap2 = PlayerCheckpoint::create();
        p1->saveToCheckpoint(snap1);
        p2->saveToCheckpoint(snap2);

        adi::simulating = true;
        adi::simDied = false;

        if (changeInput) GJBaseGameLayer::handleButton(hold, 1, isP1);

        for (int i = 0; i < steps && !adi::simDied; ++i) {
            p1->update(adi::st.dt);
            this->checkCollisions(p1, adi::st.dt, false);
            if (m_gameState.m_isDualMode) {
                p2->update(adi::st.dt);
                this->checkCollisions(p2, adi::st.dt, true);
            }
        }

        p1->loadFromCheckpoint(snap1);
        p2->loadFromCheckpoint(snap2);

        // Devolver el botón a lo que el juego creía antes (aún dentro de la simulación)
        if (changeInput) {
            GJBaseGameLayer::handleButton(adi::st.applied[isP1 ? 0 : 1], 1, isP1);
        }

        adi::simulating = false;
        return adi::simDied;
    }

    void applyInput(int i, bool down) {
        adi::st.applied[i] = down;
        adi::st.pending[i] = false;
        GJBaseGameLayer::handleButton(down, 1, i == 0);
    }

    // Aquí llega cada toque/release en el paso de física correcto.
    void handleButton(bool down, int button, bool isP1) {
        int i = isP1 ? 0 : 1;

        bool passthrough = !adi::enabled()
            || button != 1                 // izquierda/derecha del platformer pasan directo
            || adi::simulating
            || !PlayLayer::get();          // editor, etc.

        if (passthrough) {
            // Mantener al día lo que el juego cree que haces
            if (button == 1 && !adi::simulating) {
                adi::st.desired[i] = down;
                adi::st.applied[i] = down;
                adi::st.pending[i] = false;
            }
            return GJBaseGameLayer::handleButton(down, button, isP1);
        }

        adi::st.desired[i] = down;
        adi::st.pending[i] = false;

        // 0) Evento repetido (el juego ya cree que estás así): pasa directo.
        if (down == adi::st.applied[i]) {
            return GJBaseGameLayer::handleButton(down, button, isP1);
        }

        int N = adi::lookahead();

        // 1) Si aplicarlo ahora es seguro -> se aplica YA (frame perfect intacto)
        if (!simulate(true, down, isP1, N)) {
            return applyInput(i, down);
        }

        // 2) Aplicarlo mata. Si quedarse como estoy también mata, no hay salida.
        if (simulate(false, adi::st.applied[i], isP1, N)) {
            return applyInput(i, down);
        }

        // 3) Hay salvación posible: el dado se tira AQUÍ, una sola vez.
        //    Si sale a favor, el input se retiene hasta el primer paso seguro.
        if (adi::roll(adi::chance())) {
            adi::st.pending[i] = true;
            adi::st.waited[i] = 0;
            return;
        }

        // 4) El dado dijo que no: el input pasa tal cual (y probablemente mueres)
        applyInput(i, down);
    }

    // Antes de cada paso, reintentar los inputs retenidos (sin volver a tirar dado).
    // En esta versión de GD la función recibe 3 argumentos.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        adi::st.dt = dt;

        if (adi::enabled() && !adi::simulating && PlayLayer::get()) {
            int N = adi::lookahead();
            for (int i = 0; i < 2; ++i) {
                if (!adi::st.pending[i]) continue;

                bool want = adi::st.desired[i];
                if (want == adi::st.applied[i]) {
                    adi::st.pending[i] = false;
                    continue;
                }

                // Tope de seguridad: nunca retener más de MAX_PENDING_STEPS pasos
                adi::st.waited[i]++;
                if (adi::st.waited[i] > adi::MAX_PENDING_STEPS) {
                    applyInput(i, want);
                    continue;
                }

                bool isP1 = (i == 0);
                if (!simulate(true, want, isP1, N)) {
                    // Primer paso seguro: aplicar
                    applyInput(i, want);
                }
                else if (simulate(false, adi::st.applied[i], isP1, N)) {
                    // Ya no hay salida: aplicar y que pase lo que tenga que pasar
                    applyInput(i, want);
                }
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};
