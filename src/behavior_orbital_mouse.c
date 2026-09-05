/*
 * Orbital Mouse behavior for ZMK.
 *
 * Port of Pascal Getreuer's QMK "orbital_mouse" community module:
 *   https://getreuer.info/posts/keyboards/orbital-mouse/index.html
 *   https://github.com/getreuer/qmk-modules/tree/main/orbital_mouse
 *
 * Original work Copyright 2023-2025 Google LLC, licensed under the Apache
 * License, Version 2.0. This port is likewise Apache-2.0.
 *
 * Tank-control mouse keys: OM_U / OM_D move forward / backward along a
 * heading, OM_L / OM_R steer. When steering, the cursor orbits a point one
 * radius ahead, which doubles as fine-scale positioning. The arithmetic is
 * fixed point (no floats at run time), exactly as in the original.
 *
 * Movement and wheel deltas are emitted through the Zephyr input subsystem, so
 * the behavior node must be referenced by a `zmk,input-listener` (see
 * dts/behaviors/orbital_mouse.dtsi). Requires CONFIG_ZMK_POINTING.
 */

#define DT_DRV_COMPAT zmk_behavior_orbital_mouse

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/hid.h>

#include <dt-bindings/zmk/orbital_mouse.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) && IS_ENABLED(CONFIG_ZMK_POINTING)

enum {
    NUM_ANGLES = 64,
    NUM_SPEED_CURVE_INTERVALS = 16,
    NUM_BUTTONS = ZMK_HID_MOUSE_NUM_BUTTONS,
};

/* Bits of `held_keys`. */
enum {
    HELD_U = 1,
    HELD_D = 2,
    HELD_L = 4,
    HELD_R = 8,
    HELD_W_U = 16,
    HELD_W_D = 32,
    HELD_W_L = 64,
    HELD_W_R = 128,
};

struct behavior_orbital_mouse_config {
    uint8_t speed_curve[NUM_SPEED_CURVE_INTERVALS];
    uint8_t radius_q6_2;      /* Orbit radius in pixels, Q6.2.          */
    uint8_t slow_move_q_8;    /* Slow-mode movement factor, Q.8.        */
    uint8_t slow_turn_q_8;    /* Slow-mode turn factor, Q.8.            */
    uint8_t fast_move_q4_4;   /* Fast-mode movement factor, Q4.4.       */
    uint8_t fast_turn_q4_4;   /* Fast-mode turn factor, Q4.4.           */
    uint8_t wheel_speed_q2_6; /* Wheel steps per frame, Q2.6.           */
    uint8_t dbl_delay_frames; /* Double-click gap in frames.            */
    uint16_t interval_ms;     /* Frame interval.                        */
};

static struct {
    const struct device *dev;
    const struct behavior_orbital_mouse_config *cfg;
    /* Fractional displacement of the cursor, Q7.8. */
    int16_t x;
    int16_t y;
    /* Fractional displacement of the wheel, Q9.6. */
    int16_t wheel_x;
    int16_t wheel_y;
    /* Current movement speed, Q9.6. */
    int16_t speed;
    /* Which movement / wheel keys are held. */
    uint8_t held_keys;
    /* Which cardinal-snapping keys are held (U=1, D=2, L=4, R=8). */
    uint8_t held_card_keys;
    /* Frames spent moving, for the speed curve. */
    uint8_t move_t;
    /* +1 forward, -1 backward. */
    int8_t move_dir;
    /* +1 counter-clockwise, -1 clockwise. */
    int8_t steer_dir;
    int8_t wheel_x_dir;
    int8_t wheel_y_dir;
    /* Heading, Q6.8: 0 = up, 16<<8 = left, 32<<8 = down, 48<<8 = right. */
    uint16_t angle;
    /* Selected mouse button, base-0. */
    uint8_t selected_button;
    /* Buttons currently reported as pressed. */
    uint8_t buttons;
    /* Double-click sequencer; 0 when idle. */
    uint8_t double_click_frame;
    bool slow;
    bool fast;
} st;

static struct k_work_delayable tick_work;

/* ---- fixed-point trig ------------------------------------------------------- */

/* sin(2*pi*phase/64) scaled by `amplitude` (Q6.2). Returns Q6.8. */
static int16_t scaled_sin(uint8_t amplitude, uint8_t phase) {
    static const uint8_t lut[NUM_ANGLES / 2] = {
        0,   25,  50,  74,  98,  120, 142, 162, 180, 197, 212, 225, 236, 244, 250, 254,
        255, 254, 250, 244, 236, 225, 212, 197, 180, 162, 142, 120, 98,  74,  50,  25};
    int16_t value = (int16_t)(((uint16_t)amplitude * lut[phase & (NUM_ANGLES / 2 - 1)] + 2) >> 2);
    return ((NUM_ANGLES / 2) & phase) == 0 ? value : -value;
}

static int16_t scaled_cos(uint8_t amplitude, uint8_t phase) {
    return scaled_sin(amplitude, phase + (NUM_ANGLES / 4));
}

/* ---- helpers ----------------------------------------------------------------- */

static void wake(void) {
    if (!k_work_delayable_is_pending(&tick_work)) {
        k_work_schedule(&tick_work, K_NO_WAIT);
    }
}

static void set_button(uint8_t i, bool pressed) {
    if (i >= NUM_BUTTONS) {
        LOG_WRN("orbital_mouse: button %u not available (ZMK exposes %u)", i + 1, NUM_BUTTONS);
        return;
    }
    const uint8_t mask = BIT(i);
    const bool was = st.buttons & mask;
    if (was == pressed) {
        return;
    }
    WRITE_BIT(st.buttons, i, pressed);
    input_report_key(st.dev, INPUT_BTN_0 + i, pressed ? 1 : 0, true, K_FOREVER);
}

static void select_button(uint8_t i) {
    if (i >= NUM_BUTTONS) {
        LOG_WRN("orbital_mouse: button %u not available (ZMK exposes %u)", i + 1, NUM_BUTTONS);
        return;
    }
    st.selected_button = i;
    /* Release everything and cancel any double click when switching selection. */
    for (uint8_t b = 0; b < NUM_BUTTONS; ++b) {
        set_button(b, false);
    }
    st.double_click_frame = 0;
}

static int8_t dir_from_held(uint8_t shift) {
    static const int8_t dir[4] = {0, 1, -1, 0};
    return dir[(st.held_keys >> shift) & 3];
}

static uint8_t card_angle_from_held(void) {
    static const uint8_t card_angles[16] = {
        /* Zero = no movement (invalid combination). */
        [HELD_U] = 0x80, /* Up; high bit set to distinguish from zero. */
        [HELD_U | HELD_L] = 1 * (NUM_ANGLES / 8),
        [HELD_L] = 2 * (NUM_ANGLES / 8),
        [HELD_D | HELD_L] = 3 * (NUM_ANGLES / 8),
        [HELD_D] = 4 * (NUM_ANGLES / 8),
        [HELD_D | HELD_R] = 5 * (NUM_ANGLES / 8),
        [HELD_R] = 6 * (NUM_ANGLES / 8),
        [HELD_U | HELD_R] = 7 * (NUM_ANGLES / 8),
    };
    return card_angles[st.held_card_keys & 0x0F];
}

/* Change heading while keeping the point one radius ahead fixed (the "orbit"). */
static void set_angle_fractional(uint16_t angle) {
    const uint8_t r = st.cfg->radius_q6_2;
    st.x += scaled_sin(r, st.angle >> 8);
    st.y += scaled_cos(r, st.angle >> 8);
    st.angle = angle;
    st.x -= scaled_sin(r, angle >> 8);
    st.y -= scaled_cos(r, angle >> 8);
    wake();
}

/* ---- frame task ------------------------------------------------------------- */

static void tick_handler(struct k_work *work) {
    ARG_UNUSED(work);
    const struct behavior_orbital_mouse_config *cfg = st.cfg;
    if (cfg == NULL) {
        return;
    }
    bool active = false;

    if (st.move_dir) {
        /* Speed follows the piecewise-linear speed curve. */
        if (st.move_t <= 16 * (NUM_SPEED_CURVE_INTERVALS - 1)) {
            if (st.move_t == 0) {
                st.speed = (int16_t)cfg->speed_curve[0] * 16;
            } else {
                const uint8_t i = (st.move_t - 1) / 16;
                st.speed += (int16_t)cfg->speed_curve[i + 1] - (int16_t)cfg->speed_curve[i];
            }
            ++st.move_t;
        }
        uint8_t speed = (st.speed + 8) / 16; /* Q9.6 -> Q6.2 */
        if (st.slow) {
            speed = ((uint16_t)speed * (1 + (uint16_t)cfg->slow_move_q_8)) >> 8;
        } else if (st.fast) {
            speed = MIN(255, ((uint16_t)speed * (1 + (uint16_t)cfg->fast_move_q4_4)) >> 4);
        }
        st.x -= st.move_dir * scaled_sin(speed, st.angle >> 8);
        st.y -= st.move_dir * scaled_cos(speed, st.angle >> 8);
        active = true;
    }

    if (st.steer_dir) {
        int16_t angle_step = st.slow ? cfg->slow_turn_q_8
                                     : (st.fast ? (int16_t)cfg->fast_turn_q4_4 << 4 : 256);
        if (st.steer_dir < 0) {
            angle_step = -angle_step;
        }
        set_angle_fractional(st.angle + angle_step);
        active = true;
    }

    if (st.wheel_x_dir || st.wheel_y_dir) {
        st.wheel_x -= st.wheel_x_dir * cfg->wheel_speed_q2_6;
        st.wheel_y += st.wheel_y_dir * cfg->wheel_speed_q2_6;
        active = true;
    }

    if (st.double_click_frame) {
        ++st.double_click_frame;
        const bool down = st.buttons & BIT(st.selected_button);
        if (st.double_click_frame == 2 || st.double_click_frame == 3 ||
            st.double_click_frame == 4 + cfg->dbl_delay_frames) {
            set_button(st.selected_button, !down);
        } else if (st.double_click_frame == 5 + cfg->dbl_delay_frames) {
            set_button(st.selected_button, false);
            st.double_click_frame = 0;
        }
        active = true;
    }

    /* Emit whole-pixel deltas, keep the fractional remainders. */
    const int16_t dx = st.x / 256;
    const int16_t dy = st.y / 256;
    st.x -= dx * 256;
    st.y -= dy * 256;
    const int16_t h = st.wheel_x / 64;
    const int16_t v = st.wheel_y / 64;
    st.wheel_x -= h * 64;
    st.wheel_y -= v * 64;

    /* Report the non-zero axes as one frame, with sync set on the last one. */
    struct rel_ev {
        uint16_t code;
        int16_t value;
    } rel[4];
    int n = 0;
    if (dx) {
        rel[n++] = (struct rel_ev){INPUT_REL_X, dx};
    }
    if (dy) {
        rel[n++] = (struct rel_ev){INPUT_REL_Y, dy};
    }
    if (h) {
        rel[n++] = (struct rel_ev){INPUT_REL_HWHEEL, h};
    }
    if (v) {
        rel[n++] = (struct rel_ev){INPUT_REL_WHEEL, v};
    }
    for (int i = 0; i < n; ++i) {
        input_report_rel(st.dev, rel[i].code, rel[i].value, i == n - 1, K_FOREVER);
    }

    if (active) {
        k_work_reschedule(&tick_work, K_MSEC(cfg->interval_ms));
    }
}

/* ---- key handling ------------------------------------------------------------ */

static uint8_t held_mask_for(uint32_t code) {
    switch (code) {
    case OM_U:
        return HELD_U;
    case OM_D:
        return HELD_D;
    case OM_L:
        return HELD_L;
    case OM_R:
        return HELD_R;
    case OM_W_U:
        return HELD_W_U;
    case OM_W_D:
        return HELD_W_D;
    case OM_W_L:
        return HELD_W_L;
    case OM_W_R:
        return HELD_W_R;
    }
    return 0;
}

static void process(const struct device *dev, uint32_t code, bool pressed) {
    st.dev = dev;
    st.cfg = dev->config;

    const uint8_t held_mask = held_mask_for(code);
    if (held_mask) {
        WRITE_BIT(st.held_keys, __builtin_ctz(held_mask), pressed);
    } else if (code >= OM_CS_U && code <= OM_CS_R) {
        WRITE_BIT(st.held_card_keys, code - OM_CS_U, pressed);
    } else if (code >= OM_BTN1 && code < OM_BTN1 + 8) {
        set_button(code - OM_BTN1, pressed);
        return;
    } else if (code >= OM_SEL1 && code < OM_SEL1 + 8) {
        if (pressed) {
            select_button(code - OM_SEL1);
        }
        return;
    } else {
        switch (code) {
        case OM_BTNS:
            set_button(st.selected_button, pressed);
            return;
        case OM_HLDS:
            if (pressed) {
                set_button(st.selected_button, true);
            }
            return;
        case OM_RELS:
            if (pressed) {
                set_button(st.selected_button, false);
            }
            return;
        case OM_DBLS:
            if (pressed) {
                st.double_click_frame = 1;
            }
            break;
        case OM_SLOW:
            st.slow = pressed;
            return;
        case OM_FAST:
            st.fast = pressed;
            return;
        default:
            LOG_WRN("orbital_mouse: unknown code %u", code);
            return;
        }
    }

    int8_t move_dir = 0;
    if (st.held_card_keys) {
        /* Cardinal snapping: heading jumps to the held direction, steering freezes. */
        const uint8_t angle = card_angle_from_held();
        if (angle) {
            st.angle = (uint16_t)(angle & (NUM_ANGLES - 1)) << 8;
            move_dir = 1;
        }
        st.steer_dir = 0;
    } else {
        move_dir = dir_from_held(0);
        st.steer_dir = dir_from_held(2);
    }
    if (st.move_dir != move_dir) {
        st.move_dir = move_dir;
        st.move_t = 0;
    }
    st.wheel_y_dir = dir_from_held(4);
    st.wheel_x_dir = dir_from_held(6);
    wake();
}

/* ---- behavior driver -------------------------------------------------------- */

static int on_binding_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    process(zmk_behavior_get_binding(binding->behavior_dev), binding->param1, true);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_binding_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    process(zmk_behavior_get_binding(binding->behavior_dev), binding->param1, false);
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
#define OM_META(name, val)                                                                         \
    { .display_name = name, .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = val }
static const struct behavior_parameter_value_metadata param_values[] = {
    OM_META("Forward", OM_U),          OM_META("Backward", OM_D),
    OM_META("Steer Left", OM_L),       OM_META("Steer Right", OM_R),
    OM_META("Up", OM_CS_U),            OM_META("Down", OM_CS_D),
    OM_META("Left", OM_CS_L),          OM_META("Right", OM_CS_R),
    OM_META("Wheel Up", OM_W_U),       OM_META("Wheel Down", OM_W_D),
    OM_META("Wheel Left", OM_W_L),     OM_META("Wheel Right", OM_W_R),
    OM_META("Slow", OM_SLOW),          OM_META("Fast", OM_FAST),
    OM_META("Click Selected", OM_BTNS), OM_META("Double Click Selected", OM_DBLS),
    OM_META("Hold Selected", OM_HLDS), OM_META("Release Selected", OM_RELS),
    OM_META("Button 1", OM_BTN1),      OM_META("Button 2", OM_BTN2),
    OM_META("Button 3", OM_BTN3),      OM_META("Button 4", OM_BTN4),
    OM_META("Button 5", OM_BTN5),      OM_META("Select Button 1", OM_SEL1),
    OM_META("Select Button 2", OM_SEL2), OM_META("Select Button 3", OM_SEL3),
    OM_META("Select Button 4", OM_SEL4), OM_META("Select Button 5", OM_SEL5),
};
static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};
static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};
#endif

static const struct behavior_driver_api behavior_orbital_mouse_driver_api = {
    .binding_pressed = on_binding_pressed,
    .binding_released = on_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

static int behavior_orbital_mouse_init(const struct device *dev) {
    static bool initialized;
    if (!initialized) {
        k_work_init_delayable(&tick_work, tick_handler);
        initialized = true;
    }
    return 0;
}

/* Percent -> fixed point, clamped to a byte. */
#define PCT_Q(n, prop, one) MIN(255, (DT_INST_PROP(n, prop) * (one)) / 100)

#define ORBITAL_MOUSE_INST(n)                                                                      \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, speed_curve) == NUM_SPEED_CURVE_INTERVALS,                   \
                 "speed-curve must have exactly 16 entries");                                      \
    BUILD_ASSERT(DT_INST_PROP(n, radius) >= 0 && DT_INST_PROP(n, radius) <= 63,                   \
                 "radius must be in [0, 63]");                                                     \
    BUILD_ASSERT(DT_INST_PROP(n, interval_ms) > 0, "interval-ms must be positive");                \
    static const struct behavior_orbital_mouse_config behavior_orbital_mouse_config_##n = {        \
        .speed_curve = DT_INST_PROP(n, speed_curve),                                               \
        .radius_q6_2 = DT_INST_PROP(n, radius) * 4,                                                \
        .slow_move_q_8 = PCT_Q(n, slow_move_percent, 256),                                         \
        .slow_turn_q_8 = PCT_Q(n, slow_turn_percent, 256),                                         \
        .fast_move_q4_4 = PCT_Q(n, fast_move_percent, 16),                                         \
        .fast_turn_q4_4 = PCT_Q(n, fast_turn_percent, 16),                                         \
        .wheel_speed_q2_6 = PCT_Q(n, wheel_speed_percent, 64),                                     \
        .dbl_delay_frames = DT_INST_PROP(n, dbl_delay_ms) / DT_INST_PROP(n, interval_ms),          \
        .interval_ms = DT_INST_PROP(n, interval_ms),                                               \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_orbital_mouse_init, NULL, NULL,                            \
                            &behavior_orbital_mouse_config_##n, POST_KERNEL,                       \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_orbital_mouse_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ORBITAL_MOUSE_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) && CONFIG_ZMK_POINTING */
