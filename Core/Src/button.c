#include "button.h"
#include "app_config.h"

void button_init(Button *button, uint32_t now_ms)
{
    button->last_press_ms = now_ms;
    button->pending_since_ms = now_ms;
    button->event_ms = now_ms;
    button->has_last_press = false;
    button->pending = false;
}

ButtonEvent button_press(Button *button, uint32_t press_ms)
{
    if (button->has_last_press &&
        (uint32_t)(press_ms - button->last_press_ms) < APP_DEBOUNCE_MS) {
        return BUTTON_NONE;
    }
    button->last_press_ms = press_ms;
    button->has_last_press = true;

    if (button->pending) {
        if ((uint32_t)(press_ms - button->pending_since_ms) < APP_DOUBLE_PRESS_MS) {
            button->pending = false;
            button->event_ms = press_ms;
            return BUTTON_DOUBLE;
        }
        /* Exactly 500 ms is two singles, not a double. Resolve the older
         * single and retain the new press for its own classification. */
        button->event_ms = button->pending_since_ms + APP_DOUBLE_PRESS_MS;
        button->pending_since_ms = press_ms;
        return BUTTON_SINGLE;
    }
    button->pending = true;
    button->pending_since_ms = press_ms;
    return BUTTON_NONE;
}

ButtonEvent button_poll(Button *button, uint32_t now_ms)
{
    if (button->pending &&
        (uint32_t)(now_ms - button->pending_since_ms) >= APP_DOUBLE_PRESS_MS) {
        button->pending = false;
        button->event_ms = button->pending_since_ms + APP_DOUBLE_PRESS_MS;
        return BUTTON_SINGLE;
    }
    return BUTTON_NONE;
}
