/*
 * Select Word behavior for ZMK.
 *
 * Port of Pascal Getreuer's QMK "select_word" community module:
 *   https://getreuer.info/posts/keyboards/select-word/index.html
 *   https://github.com/getreuer/qmk-modules/tree/main/select_word
 *
 * Original work Copyright 2021-2025 Google LLC, licensed under the
 * Apache License, Version 2.0. This port is likewise Apache-2.0.
 *
 * Behavior:
 *   &select_word SW_WORD     Select the current word; repeat/hold to extend forward.
 *   &select_word SW_BACK     Select the word left of the cursor; repeat to extend back.
 *   &select_word SW_LINE     Select the current line; repeat/hold to extend downward.
 *   &select_word SW_LINE_UP  Select the current line; repeat/hold to extend upward.
 *
 * SW_WORD with a Shift key held behaves like SW_LINE, matching the QMK module.
 * The `mac` devicetree property switches to macOS hotkeys.
 */

#define DT_DRV_COMPAT zmk_behavior_select_word

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include "zmk_compat.h"
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

#include <dt-bindings/zmk/select_word.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct behavior_select_word_config {
    bool mac;
    uint32_t timeout_ms;
};

/*
 * Selection state is intentionally global (not per instance): it describes the
 * text selection on the host, which is shared by every instance of the behavior.
 */
static struct {
    /* 0: no selection. +1/-1: forward/backward word. +2/-2: downward/upward line. */
    int8_t selection_dir;
    /* HID usage currently held as the "extend" hotkey, or 0 if none. */
    uint32_t held_usage;
    /* Explicit modifiers we registered together with held_usage. */
    zmk_mod_flags_t held_mods;
    /* Config of the instance that registered the current hotkey. */
    const struct behavior_select_word_config *cfg;
} state;

static struct k_work_delayable idle_work;

/* ---- small HID helpers ---------------------------------------------------- */

static inline uint32_t bare_usage(uint32_t encoded) {
    return ZMK_HID_USAGE(ZMK_HID_USAGE_PAGE(encoded), ZMK_HID_USAGE_ID(encoded));
}

static inline void send(void) { ZMK_COMPAT_SEND_REPORT(HID_USAGE_KEY); }

/* Tap an encoded key such as LS(END): mods + key down in one report, up in the next. */
static void tap(uint32_t encoded) {
    const zmk_mod_flags_t mods = SELECT_MODS(encoded);
    const uint32_t usage = bare_usage(encoded);
    if (mods) {
        zmk_hid_register_mods(mods);
    }
    zmk_hid_press(usage);
    send();
    zmk_hid_release(usage);
    if (mods) {
        zmk_hid_unregister_mods(mods);
    }
    send();
}

/* Temporarily drop the user's explicit modifiers so they don't corrupt the hotkeys. */
static zmk_mod_flags_t mask_mods(void) {
    const zmk_mod_flags_t saved = zmk_hid_get_explicit_mods();
    if (saved) {
        zmk_hid_unregister_mods(saved);
    }
    return saved;
}

static void restore_mods(zmk_mod_flags_t saved) {
    if (saved) {
        zmk_hid_register_mods(saved);
    }
}

/* Hold a hotkey (mods + key) until select_word_unregister() is called. */
static void hold(zmk_mod_flags_t mods, uint32_t encoded_key) {
    state.held_mods = mods;
    state.held_usage = bare_usage(encoded_key);
    zmk_hid_register_mods(mods);
    zmk_hid_press(state.held_usage);
    send();
    /* Like QMK's set_mods(saved_mods): the mods stay pressed on the host until the
     * next report, which will be the release of the hotkey. */
    zmk_hid_unregister_mods(mods);
}

/* ---- selection logic (mirrors select_word.c) ------------------------------ */

static void select_word_in_dir(int8_t dir) {
    /* Windows/Linux: Ctrl+Right, Ctrl+Left, then hold Ctrl+Shift+Right (fwd) or
     * Ctrl+Left, Ctrl+Right, then hold Ctrl+Shift+Left (back). Mac: Alt instead
     * of Ctrl. To extend an existing selection only the held hotkey is sent. */
    const bool mac = state.cfg->mac;
    const zmk_mod_flags_t saved = mask_mods();

    if (state.selection_dir && (state.selection_dir < 0) != (dir < 0)) {
        /* Reversal: collapse the selection to the far end first. */
        send();
        tap((dir < 0) ? RIGHT : LEFT);
    }

    const zmk_mod_flags_t word_mod = mac ? MOD_LALT : MOD_LCTL;
    if (state.selection_dir == 0) {
        /* Initial selection: jump to the start (fwd) or end (back) of the word. */
        zmk_hid_register_mods(word_mod);
        send();
        if (dir < 0) {
            tap(LEFT);
            tap(RIGHT);
        } else {
            tap(RIGHT);
            tap(LEFT);
        }
        zmk_hid_unregister_mods(word_mod);
    }

    hold(word_mod | MOD_LSFT, (dir < 0) ? LEFT : RIGHT);
    restore_mods(saved);
    state.selection_dir = dir;
}

static void select_line(int8_t dir) {
    /* Windows/Linux: Home, Shift+End (down) or End, Shift+Home (up); Mac uses
     * Cmd+Left/Right. Extending an existing line selection holds Shift+Down/Up. */
    const bool mac = state.cfg->mac;
    const zmk_mod_flags_t saved = mask_mods();

    if (state.selection_dir != dir) {
        send();
        if (state.selection_dir && (state.selection_dir < 0) != (dir < 0)) {
            tap((dir < 0) ? LEFT : RIGHT); /* Reversal. */
            state.selection_dir = 0;
        }
        if (state.selection_dir == 0) {
            /* Move the cursor to the start/end of the line. */
            tap(mac ? ((dir < 0) ? LG(RIGHT) : LG(LEFT)) : ((dir < 0) ? END : HOME));
        }
        /* Select to the opposite end of the line. */
        tap(mac ? ((dir < 0) ? LS(LG(LEFT)) : LS(LG(RIGHT))) : ((dir < 0) ? LS(HOME) : LS(END)));
    } else {
        hold(MOD_LSFT, (dir < 0) ? UP : DOWN);
    }

    restore_mods(saved);
    state.selection_dir = dir;
}

static void select_word_unregister(void) {
    if (!state.held_usage) {
        goto schedule;
    }

    const uint32_t usage = state.held_usage;
    const bool mac = state.cfg ? state.cfg->mac : false;
    state.held_usage = 0;
    state.held_mods = 0;

    zmk_hid_release(usage);

    if (usage == bare_usage(DOWN) || usage == bare_usage(UP)) {
        /* After extending by lines, make sure the selection covers the whole
         * current line: Shift+End (down) or Shift+Home (up); Cmd+Shift+arrow on Mac. */
        const zmk_mod_flags_t saved = mask_mods();
        send();
        if (usage == bare_usage(DOWN)) {
            tap(mac ? LG(LS(RIGHT)) : LS(END));
        } else {
            tap(mac ? LG(LS(LEFT)) : LS(HOME));
        }
        restore_mods(saved);
        if (saved) {
            send();
        }
    } else {
        send();
    }

schedule:
    if (state.cfg && state.cfg->timeout_ms > 0 && state.selection_dir) {
        k_work_reschedule(&idle_work, K_MSEC(state.cfg->timeout_ms));
    }
}

static void select_word_register(const struct behavior_select_word_config *cfg, uint32_t action) {
    if (state.held_usage) {
        select_word_unregister(); /* Another instance is still held. */
    }
    k_work_cancel_delayable(&idle_work);
    state.cfg = cfg;

    if (action == SW_WORD && (zmk_hid_get_explicit_mods() & (MOD_LSFT | MOD_RSFT))) {
        action = SW_LINE; /* Shift + SW_WORD = line selection, as in QMK. */
    }

    switch (action) {
    case SW_WORD:
        select_word_in_dir(1);
        break;
    case SW_BACK:
        select_word_in_dir(-1);
        break;
    case SW_LINE:
        select_line(2);
        break;
    case SW_LINE_UP:
        select_line(-2);
        break;
    default:
        LOG_WRN("select_word: unknown action %u", action);
        break;
    }
}

static void idle_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    state.selection_dir = 0;
}

/* ---- behavior driver -------------------------------------------------------- */

static int on_binding_pressed(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    select_word_register(dev->config, binding->param1);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_binding_released(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event) {
    select_word_unregister();
    return ZMK_BEHAVIOR_OPAQUE;
}

/* Any ordinary key press or release on the host invalidates our idea of the selection. */
static int select_word_keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || state.selection_dir == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (!is_mod(ev->usage_page, ev->keycode)) {
        state.selection_dir = 0;
        k_work_cancel_delayable(&idle_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_select_word, select_word_keycode_listener);
ZMK_SUBSCRIPTION(behavior_select_word, zmk_keycode_state_changed);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata param_values[] = {
    {.display_name = "Word", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = SW_WORD},
    {.display_name = "Word Back", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = SW_BACK},
    {.display_name = "Line", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = SW_LINE},
    {.display_name = "Line Up", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = SW_LINE_UP},
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

static const struct behavior_driver_api behavior_select_word_driver_api = {
    .binding_pressed = on_binding_pressed,
    .binding_released = on_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

static int behavior_select_word_init(const struct device *dev) {
    static bool initialized;
    if (!initialized) {
        k_work_init_delayable(&idle_work, idle_work_handler);
        initialized = true;
    }
    return 0;
}

#define SELECT_WORD_INST(n)                                                                        \
    static const struct behavior_select_word_config behavior_select_word_config_##n = {            \
        .mac = DT_INST_PROP(n, mac),                                                               \
        .timeout_ms = DT_INST_PROP(n, timeout_ms),                                                 \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_select_word_init, NULL, NULL,                              \
                            &behavior_select_word_config_##n, POST_KERNEL,                         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_select_word_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SELECT_WORD_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
