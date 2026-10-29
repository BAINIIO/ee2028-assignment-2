/* EE2028 Assignment 2: compulsory drone controls + zombie game enhancement. */
#include "main.h"
#include "app_config.h"
#include "board_io.h"
#include "button.h"
#include "external_io.h"
#include "game.h"
#include "telemetry.h"
#include "command_telemetry.h"
#include "nfc_io.h"
#include <stdio.h>

#define BUTTON_QUEUE_CAPACITY 32u
static volatile uint32_t button_edges[BUTTON_QUEUE_CAPACITY];
static volatile uint8_t edge_write;
static volatile uint8_t edge_read;
static volatile bool edge_overflow;
static Game game;
static Button button;
static SensorSample sensors;
static TelemetryStatus telemetry_status;
static uint32_t last_attack_log;
static uint32_t dropped_uart_messages;
static bool terminal_event;
/* Presentation state follows resolved game events; it never delays controls. */
static ExternalDisplayEffect display_effect;
static ExternalColour display_effect_target;
static uint32_t display_effect_since;
static uint32_t display_target_since;
static uint16_t display_effect_target_age;

static uint16_t display_age(uint32_t elapsed)
{
    return elapsed > UINT16_MAX ? UINT16_MAX : (uint16_t)elapsed;
}

static ExternalColour target_colour(GameTarget target)
{
    return target == GAME_TARGET_ZOMBIE ? EXTERNAL_COLOUR_RED :
           target == GAME_TARGET_HUMAN ? EXTERNAL_COLOUR_GREEN : EXTERNAL_COLOUR_NONE;
}

static void display_event(const GameEvent *event)
{
    ExternalDisplayEffect effect = EXTERNAL_EFFECT_NONE;
    switch (event->type) {
    case GAME_EVENT_STANDBY:
    case GAME_EVENT_BATTLE:
        display_effect = EXTERNAL_EFFECT_NONE;
        display_target_since = event->time_ms;
        return;
    case GAME_EVENT_TARGET:
        display_target_since = event->time_ms;
        return;
    case GAME_EVENT_CHARGED: effect = EXTERNAL_EFFECT_CHARGE; break;
    case GAME_EVENT_FIRED: effect = EXTERNAL_EFFECT_SHOT; break;
    case GAME_EVENT_HIT: effect = EXTERNAL_EFFECT_HIT; break;
    case GAME_EVENT_HUMAN_HIT: effect = EXTERNAL_EFFECT_HUMAN_HIT; break;
    case GAME_EVENT_HUMAN_SAVED: effect = EXTERNAL_EFFECT_HUMAN_SAVED; break;
    case GAME_EVENT_MISSED: effect = EXTERNAL_EFFECT_MISS; break;
    case GAME_EVENT_RESCUED:
        effect = EXTERNAL_EFFECT_RESCUED;
        display_target_since = event->time_ms;
        break;
    default: return;
    }
    display_effect = effect;
    display_effect_target = target_colour(event->target);
    display_effect_since = event->time_ms;
    display_effect_target_age = display_age(event->time_ms - display_target_since);
}

/* EXTI records timestamps only. No I2C, UART or debounce waits in the ISR. */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if (pin == 0x0010u) { Nfc_SignalEdge(HAL_GetTick()); return; }
    if (pin != BUTTON_EXTI13_Pin) return;
    uint8_t next = (uint8_t)((edge_write + 1u) % BUTTON_QUEUE_CAPACITY);
    if (next == edge_read) {
        edge_overflow = true;
        return;
    }
    button_edges[edge_write] = HAL_GetTick();
    __DMB();
    edge_write = next;
}

static void uart_message(const char *text)
{
    if (!Board_UartWrite(text)) ++dropped_uart_messages;
}

static void handle_buttons(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    bool overflow = edge_overflow;
    if (overflow) {
        edge_read = edge_write;
        edge_overflow = false;
    }
    __set_PRIMASK(primask);
    if (overflow) {
        button_init(&button, HAL_GetTick());
        uart_message("WARNING button event queue overflow; input discarded\r\n");
    }
    for (;;) {
        while (edge_read != edge_write) {
            uint32_t edge_ms = button_edges[edge_read];
            __DMB();
            edge_read = (uint8_t)((edge_read + 1u) % BUTTON_QUEUE_CAPACITY);
            ButtonEvent event = button_press(&button, edge_ms);
            if (event != BUTTON_NONE) game_button(&game, event, button.event_ms);
        }
        /* An edge at 499 ms must not lose to a 500 ms timeout between the
         * empty-queue check and poll. Resolve only with that check atomic. */
        primask = __get_PRIMASK();
        __disable_irq();
        if (edge_read != edge_write) {
            __set_PRIMASK(primask);
            continue;
        }
        ButtonEvent event = button_poll(&button, HAL_GetTick());
        uint32_t event_ms = button.event_ms;
        __set_PRIMASK(primask);
        if (event != BUTTON_NONE) game_button(&game, event, event_ms);
        break;
    }
}

static const char *target_name(GameTarget target)
{
    if (target == GAME_TARGET_ZOMBIE) return "ZOMBIE";
    if (target == GAME_TARGET_HUMAN) return "HUMAN";
    return "NONE";
}

static void handle_events(uint32_t now)
{
    GameEvent event;
    char message[160];
    while (game_pop_event(&game, &event)) {
        Command_Event(&event);
        display_event(&event);
        switch (event.type) {
        case GAME_EVENT_STANDBY:
            uart_message("Entering STANDBY MODE\r\n");
            break;
        case GAME_EVENT_BATTLE:
            uart_message("Entering BATTLE MODE\r\n");
            External_Play(EXTERNAL_SOUND_RESCUE, now);
            break;
        case GAME_EVENT_CHARGED:
            (void)snprintf(message, sizeof message,
                "LASERGUN_CHARGING: %u/10\r\n", (unsigned)event.energy);
            uart_message(message);
            External_Play(EXTERNAL_SOUND_CHARGE, now);
            break;
        case GAME_EVENT_FIRED:
            (void)snprintf(message, sizeof message,
                "LASERGUN_FIRED: -5/10; remaining %u/10; target=%s\r\n",
                (unsigned)event.energy, target_name(event.target));
            uart_message(message);
            External_Play(EXTERNAL_SOUND_SHOT, now);
            break;
        case GAME_EVENT_HIT:
        case GAME_EVENT_HUMAN_HIT:
        case GAME_EVENT_HUMAN_SAVED:
        case GAME_EVENT_MISSED:
            (void)snprintf(message, sizeof message, "%s; score=%d\r\n",
                event.type == GAME_EVENT_HIT ? "Zombie hit" :
                event.type == GAME_EVENT_HUMAN_HIT ? "Human hit: penalty" :
                event.type == GAME_EVENT_HUMAN_SAVED ? "Human saved" :
                "Zombie escaped: penalty", (int)event.score);
            uart_message(message);
            if (event.type == GAME_EVENT_HUMAN_HIT || event.type == GAME_EVENT_MISSED)
                External_Play(EXTERNAL_SOUND_WARNING, now);
            break;
        case GAME_EVENT_TARGET:
            (void)snprintf(message, sizeof message, "TARGET: %s\r\n",
                target_name(event.target));
            uart_message(message);
            break;
        case GAME_EVENT_COLOUR_MISMATCH:
            uart_message("Optional RGB sensor disagrees with OLED target\r\n");
            break;
        case GAME_EVENT_ATTACKED:
            uart_message("Drone Was Attacked! \r\n");
            last_attack_log = event.time_ms;
            External_Play(EXTERNAL_SOUND_ATTACK, now);
            break;
        case GAME_EVENT_RESCUED:
            uart_message("Drone rescued; resuming BATTLE MODE\r\n");
            External_Play(EXTERNAL_SOUND_RESCUE, now);
            break;
        case GAME_EVENT_VICTORY:
            uart_message("VICTORY! Six zombies defeated. Double press for standby.\r\n");
            External_Play(EXTERNAL_SOUND_WIN, now);
            break;
        case GAME_EVENT_DEFEAT:
            if (game.mode != GAME_TERMINATED)
                uart_message("DEFEAT! No lives left. Double press for standby.\r\n");
            External_Play(EXTERNAL_SOUND_LOSE, now);
            break;
        case GAME_EVENT_TERMINATED:
            uart_message("TERMINATED: rescue timed out. Communication ending.\r\n");
            terminal_event = true;
            break;
        }
    }
}

static void update_display(uint32_t now)
{
    ExternalDisplayScene scene = EXTERNAL_SCENE_STANDBY;
    const char *status = "DOUBLE TO START";
    const char *mode = "STANDBY";
    const char *hint = "DOUBLE: START";
    if (game.mode == GAME_BATTLE) {
        scene = game.attacked ? EXTERNAL_SCENE_ATTACKED :
                game.round == GAME_ROUND_WON ? EXTERNAL_SCENE_WON :
                game.round == GAME_ROUND_LOST ? EXTERNAL_SCENE_LOST : EXTERNAL_SCENE_BATTLE;
        mode = "BATTLE";
        hint = "DOUBLE: STANDBY";
        status = game.target == GAME_TARGET_HUMAN ? "HOLD FIRE" : "PRESS TO CHARGE";
        if (game.round == GAME_ROUND_WON) status = "VICTORY!";
        if (game.round == GAME_ROUND_LOST) status = "DEFEAT!";
        if (game.attacked) {
            status = "DOUBLE TO RESCUE";
            hint = "DOUBLE: RESCUE";
        }
    } else if (game.mode == GAME_TERMINATED) {
        scene = EXTERNAL_SCENE_TERMINATED;
        mode = "TERMINATED";
        status = "RESCUE FAILED";
        hint = "RESET TO RESTART";
    }
    ExternalDisplayFrame frame = {
        .mode = mode, .status = status, .hint = hint, .score = game.score,
        .energy = game.energy, .lives = game.lives,
        .rescue_seconds = (uint8_t)((game_rescue_remaining_ms(&game, now) + 999u) / 1000u),
        .target = game.attacked ? EXTERNAL_COLOUR_NONE : target_colour(game.target),
        .attacked = game.attacked && game.mode == GAME_BATTLE,
        .sensor_warning = (telemetry_status.abnormal | telemetry_status.unavailable) != 0u,
        .aim_locked = game.aim_locked,
        .scene = scene, .now_ms = now,
        .target_age_ms = display_age(now - game.target_since_ms),
        .target_period_ms = GAME_TARGET_PERIOD_MS,
        .effect_age_ms = display_age(now - display_effect_since),
        .effect_target_age_ms = display_effect_target_age,
        .effect = display_effect, .effect_target = display_effect_target,
        .zombie_hits = game.zombie_hits, .goal_hits = GAME_WIN_HITS
    };
    External_SetDisplay(&frame);
}

static void shutdown_system(void)
{
    Board_LedWrite(false);
    Command_Final(&game, &sensors, telemetry_status, HAL_GetTick());
    update_display(HAL_GetTick());
    /* The full UART queue needs about 356 ms on the wire at 115200 8N1.
     * Allow the final OLED transfer too; finish sooner when both are idle. */
    uint32_t start = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - start) < APP_SHUTDOWN_DRAIN_MS) {
        Board_UartPoll();
        if ((uint32_t)(HAL_GetTick() - start) >= APP_SHUTDOWN_DRAIN_MS) break;
        External_FinishDisplay();
        Command_Finish(HAL_GetTick());
        if (Board_UartIdle() && External_DisplayIdle() && Command_Idle()) break;
    }
    Command_Stop();
    Board_UartStop();
    External_AllStop();
    HAL_NVIC_DisableIRQ(EXTI15_10_IRQn);
    HAL_SuspendTick();
    __disable_irq();
    for (;;) __WFI(); /* Hardware reset is the only restart. */
}

static bool due(uint32_t now, uint32_t *previous, uint32_t interval)
{
    if ((uint32_t)(now - *previous) < interval) return false;
    *previous += interval;
    if ((uint32_t)(now - *previous) >= interval) *previous = now;
    return true;
}

int main(void)
{
    HAL_Init();
    if (!Board_Init()) Error_Handler();
    Command_Init(HAL_GetTick());
    External_Init(HAL_GetTick());
    uint32_t now = HAL_GetTick();
    edge_read = edge_write; /* Discard presses during boot. */
    button_init(&button, now);
    game_init(&game, now);
    display_effect = EXTERNAL_EFFECT_NONE;
    display_effect_target = EXTERNAL_COLOUR_NONE;
    display_effect_since = now;
    display_target_since = now;
    display_effect_target_age = 0u;
    handle_events(now);
    char message[160];
    (void)snprintf(message, sizeof message,
        "Zombie game ready; sensors=0x%02lx/0x3f; OLED=%s; IR=%s; buzzer=%s\r\n",
        (unsigned long)Board_SensorHealth(),
        External_DisplayOnline() ? "online" : "absent",
        External_IREnabled() ? "enabled" : "disabled",
        External_BuzzerOnline() ? "configured" : "disabled");
    uart_message(message);
    uart_message("Keep board still and upright at boot to calibrate orientation.\r\n");
    Board_ReadSensors(&sensors);
    handle_buttons();
    game_update_sensors(&game, &sensors, HAL_GetTick());
    telemetry_status = Telemetry_Check(&sensors);
    uint32_t last_sensors = now;
    uint32_t last_motion = now;
    uint32_t last_display = now - APP_DISPLAY_PERIOD_MS;
    bool previous_warning = false;
    bool previous_display_online = External_DisplayOnline();

    for (;;) {
        handle_buttons();
        now = HAL_GetTick();
        game_tick(&game, now);
        handle_events(now);
        if (terminal_event || game.mode == GAME_TERMINATED) shutdown_system();

        if (due(now, &last_motion, APP_MOTION_PERIOD_MS)) {
            Board_ReadMotion(&sensors);
            handle_buttons(); /* Edges captured during I2C reads precede time advance. */
            game_update_sensors(&game, &sensors, HAL_GetTick());
        }
        now = HAL_GetTick();
        if (due(now, &last_sensors, APP_SENSOR_PERIOD_MS)) {
            Board_ReadSensors(&sensors);
            handle_buttons();
            now = HAL_GetTick();
            game_tick(&game, now);
            handle_events(now);
            if (game.mode == GAME_TERMINATED) shutdown_system();
            telemetry_status = Telemetry_Check(&sensors);
            char telemetry[1024];
            if (Telemetry_Format(&sensors, telemetry, sizeof telemetry)) uart_message(telemetry);
            bool warning = (telemetry_status.abnormal | telemetry_status.unavailable) != 0u;
            if (warning && !previous_warning) External_Play(EXTERNAL_SOUND_WARNING, now);
            previous_warning = warning;
            if (dropped_uart_messages != 0u) {
                (void)snprintf(message, sizeof message,
                    "WARNING UART backlog: %lu messages dropped\r\n",
                    (unsigned long)dropped_uart_messages);
                if (Board_UartWrite(message)) dropped_uart_messages = 0u;
            }
        }
        now = HAL_GetTick();
        handle_events(now);
        if (terminal_event || game.mode == GAME_TERMINATED) shutdown_system();
        if (game.attacked && (uint32_t)(now - last_attack_log) >= APP_SENSOR_PERIOD_MS) {
            last_attack_log += APP_SENSOR_PERIOD_MS;
            uart_message("Drone Was Attacked! \r\n");
        }

        External_Poll(now);
        bool display_online = External_DisplayOnline();
        if (display_online != previous_display_online) {
            uart_message(display_online ? "DISPLAY: OLED recovered\r\n" :
                "DISPLAY: OLED transfer failed; retry pending\r\n");
            previous_display_online = display_online;
        }
        game_set_aim_locked(&game, External_IRPresent());
        ExternalColourSample colour = External_GetColour();
        game_set_colour(&game, !colour.valid ? GAME_COLOUR_UNKNOWN :
            colour.colour == EXTERNAL_COLOUR_RED ? GAME_COLOUR_RED :
            colour.colour == EXTERNAL_COLOUR_GREEN ? GAME_COLOUR_GREEN : GAME_COLOUR_UNKNOWN);
        if (due(HAL_GetTick(), &last_display, APP_DISPLAY_PERIOD_MS)) update_display(HAL_GetTick());
        Board_LedWrite(game_led(&game, HAL_GetTick()));
        Command_Poll(&game, &sensors, telemetry_status, HAL_GetTick());
        Board_UartPoll();
    }
}

void Error_Handler(void)
{
    /* Clock/UART startup failure: never advertise a running game. */
    __disable_irq();
    Board_LedWrite(true);
    for (;;) __WFI();
}
