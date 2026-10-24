#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BUTTON_NONE = 0,
    BUTTON_SINGLE,
    BUTTON_DOUBLE
} ButtonEvent;

/* Call from the main loop with press-edge timestamps collected by EXTI.
 * Never call these methods concurrently from the interrupt and main loop.
 * A pending first press does nothing until the strictly-less-than-500-ms
 * double-press window expires. Unsigned subtraction supports tick rollover. */
typedef struct {
    uint32_t last_press_ms;
    uint32_t pending_since_ms;
    /* Timestamp for the most recently returned non-NONE event: second edge
     * for doubles, first edge + window for singles. */
    uint32_t event_ms;
    bool has_last_press;
    bool pending;
} Button;

void button_init(Button *button, uint32_t now_ms);
ButtonEvent button_press(Button *button, uint32_t press_ms);
ButtonEvent button_poll(Button *button, uint32_t now_ms);

#endif
