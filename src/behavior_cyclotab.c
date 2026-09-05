/*
 * Cyclotab behavior for ZMK.
 *
 * Port of Pascal Getreuer's QMK "cyclotab" community module:
 *   https://getreuer.info/posts/keyboards/cyclotab/index.html
 *   https://github.com/getreuer/qmk-modules/tree/main/cyclotab
 *
 * Original work Copyright 2025 Google LLC, licensed under the Apache License,
 * Version 2.0. This port is likewise Apache-2.0.
 *
 * Usage: &cyclotab LA(TAB), &cyclotab LS(LA(TAB)), &cyclotab LG(TAB), ...
 *
 * On press, the hotkey's non-Shift modifiers are registered and kept held after
 * the key is released, so the key can be tapped repeatedly to cycle windows.
 * Shift is applied only to the individual tap (reverse direction). The held
 * modifiers are released when:
 *   - any other key is pressed (that key press is swallowed, unless the
 *     `pass-through` property is set) or released,
 *   - Escape is pressed (Escape is sent first, so the switcher cancels),
 *   - the layer the behavior was pressed on is deactivated,
 *   - `timeout-ms` elapses with nothing pressed (0 disables the timeout).
 * Shift and arrow keys can be used freely while active, as can other
 * modifiers. Tapping the hotkey's key again (e.g. via &key_repeat) continues.
 */

#define DT_DRV_COMPAT zmk_behavior_cyclotab

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include "zmk_compat.h"
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define SHIFT_MODS (MOD_LSFT | MOD_RSFT)

struct behavior_cyclotab_config {
    uint32_t timeout_ms;
    bool pass_through;
};

/* Global: there is only one set of held modifiers on the host. */
static struct {
    bool active;
    const struct behavior_cyclotab_config *cfg;
    /* Explicit modifiers currently registered on behalf of the hotkey. */
    zmk_mod_flags_t hold_mods;
    /* Bare HID usage of the hotkey's key (e.g. Tab). */
    uint32_t key_usage;
    /* Encoded keycode we currently hold via a keycode event, or 0. */
    uint32_t key_down_encoded;
    /* Layer id the behavior was pressed on; leaving it ends the cycle. */
    int layer;
    /* A Shift key is physically held: pauses the timeout. */
    bool shift_held;
    /* Bare usage whose press we swallowed; swallow the matching release too. */
    uint32_t consumed_usage;
} state;

static struct k_work_delayable release_work;

static inline uint32_t bare_usage(uint32_t encoded) {
    return ZMK_HID_USAGE(ZMK_HID_USAGE_PAGE(encoded), ZMK_HID_USAGE_ID(encoded));
}

static inline void send(void) { ZMK_COMPAT_SEND_REPORT(HID_USAGE_KEY); }

static void update_timer(bool pressed) {
    if (!state.active || pressed || state.shift_held || state.cfg->timeout_ms == 0) {
        k_work_cancel_delayable(&release_work); /* Pause. */
    } else {
        k_work_reschedule(&release_work, K_MSEC(state.cfg->timeout_ms));
    }
}

static void release_active(void) {
    if (!state.active) {
        return;
    }
    state.active = false;
    k_work_cancel_delayable(&release_work);

    if (state.key_down_encoded) {
        raise_zmk_keycode_state_changed_from_encoded(state.key_down_encoded, false,
                                                     k_uptime_get());
        state.key_down_encoded = 0;
    }
    if (state.hold_mods) {
        zmk_hid_unregister_mods(state.hold_mods);
        send();
    }
    state.hold_mods = 0;
    state.shift_held = false;
}

static void release_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    release_active();
}

/* ---- behavior driver -------------------------------------------------------- */

static int on_binding_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_cyclotab_config *cfg = dev->config;

    const uint32_t encoded = binding->param1;
    const zmk_mod_flags_t mods = SELECT_MODS(encoded);
    const zmk_mod_flags_t hold_mods = mods & ~SHIFT_MODS;
    const zmk_mod_flags_t tap_mods = mods & SHIFT_MODS;
    const uint32_t usage = bare_usage(encoded);

    if (state.active && state.hold_mods != hold_mods) {
        release_active(); /* Switching to a hotkey with different modifiers. */
    }

    if (!state.active) {
        state.active = true;
        state.cfg = cfg;
        state.hold_mods = hold_mods;
        state.layer = event.layer;
        state.shift_held = false;
        state.consumed_usage = 0;
        if (hold_mods) {
            zmk_hid_register_mods(hold_mods);
            send();
        }
    }
    state.key_usage = usage;

    /* Send the key itself as a normal keycode event so it is visible to other
     * behaviors (e.g. &key_repeat) and released cleanly by hid_listener. */
    if (state.key_down_encoded) {
        raise_zmk_keycode_state_changed_from_encoded(state.key_down_encoded, false,
                                                     event.timestamp);
    }
    state.key_down_encoded = ((uint32_t)tap_mods << 24) | usage;
    raise_zmk_keycode_state_changed_from_encoded(state.key_down_encoded, true, event.timestamp);
    update_timer(true);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_binding_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    if (state.key_down_encoded) {
        raise_zmk_keycode_state_changed_from_encoded(state.key_down_encoded, false,
                                                     event.timestamp);
        state.key_down_encoded = 0;
    }
    update_timer(false);
    return ZMK_BEHAVIOR_OPAQUE;
}

/* ---- listeners ---------------------------------------------------------------- */

static bool is_arrow(uint16_t page, uint32_t id) {
    return page == HID_USAGE_KEY &&
           (id == HID_USAGE_KEY_KEYBOARD_RIGHTARROW || id == HID_USAGE_KEY_KEYBOARD_LEFTARROW ||
            id == HID_USAGE_KEY_KEYBOARD_DOWNARROW || id == HID_USAGE_KEY_KEYBOARD_UPARROW);
}

static bool is_shift(uint16_t page, uint32_t id) {
    return page == HID_USAGE_KEY &&
           (id == HID_USAGE_KEY_KEYBOARD_LEFTSHIFT || id == HID_USAGE_KEY_KEYBOARD_RIGHTSHIFT);
}

static int cyclotab_keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    const uint32_t usage = ZMK_HID_USAGE(ev->usage_page, ev->keycode);

    /* Release of a key whose press we swallowed: swallow it too. */
    if (!ev->state && state.consumed_usage && usage == state.consumed_usage) {
        state.consumed_usage = 0;
        return ZMK_EV_EVENT_HANDLED;
    }
    if (!state.active) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (is_shift(ev->usage_page, ev->keycode)) {
        state.shift_held = ev->state; /* Reverse direction; pauses the timeout. */
        update_timer(ev->state);
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (is_mod(ev->usage_page, ev->keycode)) {
        return ZMK_EV_EVENT_BUBBLE; /* Other modifiers pass through untouched. */
    }
    if (is_arrow(ev->usage_page, ev->keycode) || usage == state.key_usage) {
        update_timer(ev->state); /* Navigation within the switcher. */
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->state && ev->usage_page == HID_USAGE_KEY &&
        ev->keycode == HID_USAGE_KEY_KEYBOARD_ESCAPE) {
        /* Tap Escape while the modifiers are still held to cancel switching. */
        zmk_hid_press(usage);
        send();
        zmk_hid_release(usage);
        send();
        release_active();
        state.consumed_usage = usage;
        return ZMK_EV_EVENT_HANDLED;
    }

    /* Any other key completes the selection. */
    const bool pass_through = state.cfg->pass_through;
    release_active();
    if (!ev->state || pass_through) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    state.consumed_usage = usage;
    return ZMK_EV_EVENT_HANDLED;
}

static int cyclotab_layer_listener(const zmk_event_t *eh) {
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev != NULL && state.active && !ev->state && ev->layer == state.layer) {
        release_active(); /* Leaving the layer releases the modifiers immediately. */
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_cyclotab, cyclotab_keycode_listener);
ZMK_SUBSCRIPTION(behavior_cyclotab, zmk_keycode_state_changed);

ZMK_LISTENER(behavior_cyclotab_layer, cyclotab_layer_listener);
ZMK_SUBSCRIPTION(behavior_cyclotab_layer, zmk_layer_state_changed);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata param_values[] = {
    {.display_name = "Hotkey", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_HID_USAGE},
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

static const struct behavior_driver_api behavior_cyclotab_driver_api = {
    .binding_pressed = on_binding_pressed,
    .binding_released = on_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

static int behavior_cyclotab_init(const struct device *dev) {
    static bool initialized;
    if (!initialized) {
        k_work_init_delayable(&release_work, release_work_handler);
        initialized = true;
    }
    return 0;
}

#define CYCLOTAB_INST(n)                                                                           \
    static const struct behavior_cyclotab_config behavior_cyclotab_config_##n = {                  \
        .timeout_ms = DT_INST_PROP(n, timeout_ms),                                                 \
        .pass_through = DT_INST_PROP(n, pass_through),                                             \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_cyclotab_init, NULL, NULL, &behavior_cyclotab_config_##n,  \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                            &behavior_cyclotab_driver_api);

DT_INST_FOREACH_STATUS_OKAY(CYCLOTAB_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
