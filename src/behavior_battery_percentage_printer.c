/*
 * Minimal ZMK behavior: types battery percentage with a label when activated.
 * Uses zmk_battery_state_of_charge().
 *
 */

#define DT_DRV_COMPAT zmk_behavior_battery_percentage_printer

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/battery.h>
#include <dt-bindings/zmk/keys.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif // IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MAX_CHARS 96
#define TYPE_DELAY_MS 10

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct behavior_battery_printer_data {
    struct k_work_delayable typing_work;
    uint8_t chars[MAX_CHARS];
    uint8_t chars_len;
    uint8_t current_idx;
    bool key_pressed;
};

struct behavior_battery_printer_config {
    bool print_all_batteries;
};

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static bool peripheral_battery_seen[ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT];
#endif

static void uint_to_chars(uint32_t v, uint8_t *buffer, uint8_t *len);

static inline void reset_typing_state(struct behavior_battery_printer_data *data) {
    data->current_idx = 0;
    data->key_pressed = false;
    data->chars_len = 0;
    memset(data->chars, 0, sizeof(data->chars));
}

/* explicit mapping for top-row digits (safe) */
static const uint32_t digit_keycodes[10] = {
    NUMBER_0, NUMBER_1, NUMBER_2, NUMBER_3, NUMBER_4,
    NUMBER_5, NUMBER_6, NUMBER_7, NUMBER_8, NUMBER_9
};

/* map an ASCII char used in our phrase to an encoded keycode understood by
 * raise_zmk_keycode_state_changed_from_encoded().
 *
 * Supports:
 *  - digits '0'..'9' -> top-row NUMBER_*
 *  - letters a..z / A..Z -> letter keycodes (uppercase uses LS(...) to require shift)
 *  - space ' ' -> SPACE
 *  - dot '.' -> DOT
 *  - percent '%' -> PERCENT (macro includes shift)
 */
static uint32_t char_to_encoded_keycode(uint8_t ch) {
    /* digits */
    if (ch >= '0' && ch <= '9') {
        return digit_keycodes[ch - '0'];
    }

    /* space */
    if (ch == ' ') {
        return SPACE;
    }

    /* dot/period */
    if (ch == '.') {
        return DOT;
    }

    /* percent sign (uses define that includes shift) */
    if (ch == '%') {
        return PERCENT;
    }

#define ALPHA_CASE(lower, upper, keycode)                                                       \
    case lower:                                                                                 \
        return keycode;                                                                         \
    case upper:                                                                                 \
        return LS(keycode)

    switch (ch) {
        ALPHA_CASE('a', 'A', A);
        ALPHA_CASE('b', 'B', B);
        ALPHA_CASE('c', 'C', C);
        ALPHA_CASE('d', 'D', D);
        ALPHA_CASE('e', 'E', E);
        ALPHA_CASE('f', 'F', F);
        ALPHA_CASE('g', 'G', G);
        ALPHA_CASE('h', 'H', H);
        ALPHA_CASE('i', 'I', I);
        ALPHA_CASE('j', 'J', J);
        ALPHA_CASE('k', 'K', K);
        ALPHA_CASE('l', 'L', L);
        ALPHA_CASE('m', 'M', M);
        ALPHA_CASE('n', 'N', N);
        ALPHA_CASE('o', 'O', O);
        ALPHA_CASE('p', 'P', P);
        ALPHA_CASE('q', 'Q', Q);
        ALPHA_CASE('r', 'R', R);
        ALPHA_CASE('s', 'S', S);
        ALPHA_CASE('t', 'T', T);
        ALPHA_CASE('u', 'U', U);
        ALPHA_CASE('v', 'V', V);
        ALPHA_CASE('w', 'W', W);
        ALPHA_CASE('x', 'X', X);
        ALPHA_CASE('y', 'Y', Y);
        ALPHA_CASE('z', 'Z', Z);
    default:
        return 0;
    }

#undef ALPHA_CASE
}

static bool append_char(struct behavior_battery_printer_data *data, uint8_t ch) {
    if (data->chars_len >= ARRAY_SIZE(data->chars)) {
        return false;
    }

    data->chars[data->chars_len++] = ch;
    return true;
}

static bool append_text(struct behavior_battery_printer_data *data, const char *text) {
    while (*text != '\0') {
        if (!append_char(data, (uint8_t)*text++)) {
            return false;
        }
    }

    return true;
}

static bool append_uint(struct behavior_battery_printer_data *data, uint32_t value) {
    uint8_t digitbuf[4];
    uint8_t digitlen = 0;

    uint_to_chars(value, digitbuf, &digitlen);
    for (uint8_t i = 0; i < digitlen; i++) {
        if (!append_char(data, digitbuf[i])) {
            return false;
        }
    }

    return true;
}

static bool append_percent_entry(struct behavior_battery_printer_data *data, uint8_t percent) {
    return append_uint(data, percent) && append_text(data, "% ");
}

static bool append_labeled_battery(struct behavior_battery_printer_data *data, const char *label,
                                   uint8_t percent) {
    return append_text(data, label) && append_char(data, ' ') && append_percent_entry(data, percent);
}

static bool append_peripheral_battery(struct behavior_battery_printer_data *data, uint8_t source,
                                      uint8_t percent) {
    return append_char(data, 'P') && append_uint(data, source) && append_char(data, ' ') &&
           append_percent_entry(data, percent);
}

static void send_key(struct behavior_battery_printer_data *data) {
    if (data->current_idx >= data->chars_len || data->current_idx >= ARRAY_SIZE(data->chars)) {
        reset_typing_state(data);
        return;
    }

    uint8_t ch = data->chars[data->current_idx];
    uint32_t keycode = char_to_encoded_keycode(ch);
    if (!keycode) {
        LOG_WRN("behavior_battery_printer: unsupported char '%c' (idx %u)", ch, data->current_idx);
        reset_typing_state(data);
        return;
    }

    bool pressed = !data->key_pressed;
    raise_zmk_keycode_state_changed_from_encoded(keycode, pressed, k_uptime_get());
    data->key_pressed = pressed;

    if (pressed) {
        /* schedule release */
        k_work_schedule(&data->typing_work, K_MSEC(TYPE_DELAY_MS));
        return;
    } else {
        /* released -> next char */
        data->current_idx++;
        if (data->current_idx < data->chars_len) {
            k_work_schedule(&data->typing_work, K_MSEC(TYPE_DELAY_MS));
        } else {
            reset_typing_state(data);
        }
    }
}

static void type_keys_work(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct behavior_battery_printer_data *data = CONTAINER_OF(dwork, struct behavior_battery_printer_data, typing_work);
    send_key(data);
}

static int behavior_battery_printer_init(const struct device *dev) {
    struct behavior_battery_printer_data *data = dev->data;
    k_work_init_delayable(&data->typing_work, type_keys_work);
    reset_typing_state(data);
    return 0;
}

/* convert small uint -> ascii digits (0..999) */
static void uint_to_chars(uint32_t v, uint8_t *buffer, uint8_t *len) {
    char tmp[4];
    int t = 0;
    if (v == 0) {
        buffer[0] = '0';
        *len = 1;
        return;
    }
    while (v > 0 && t < (int)sizeof(tmp)) {
        tmp[t++] = '0' + (v % 10);
        v /= 10;
    }
    for (int i = 0; i < t; i++) {
        buffer[i] = tmp[t - 1 - i];
    }
    *len = t;
}

static int build_single_battery_output(struct behavior_battery_printer_data *data,
                                       struct zmk_behavior_binding_event event) {
    uint8_t percent = zmk_battery_state_of_charge();

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    if (event.source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
        zmk_split_central_get_peripheral_battery_level(event.source, &percent);
    }
#endif

    if (percent > 100) {
        percent = 100;
    }

    LOG_INF("behavior_battery_printer: battery SOC read = %u%%", percent);

    if (!append_percent_entry(data, percent)) {
        LOG_ERR("behavior_battery_printer: buffer too small for single battery output");
        return -ENOMEM;
    }

    return 0;
}

static int build_all_batteries_output(struct behavior_battery_printer_data *data) {
    uint8_t percent = zmk_battery_state_of_charge();

    if (percent > 100) {
        percent = 100;
    }

    if (!append_labeled_battery(data, "C", percent)) {
        LOG_ERR("behavior_battery_printer: buffer too small for all battery output");
        return -ENOMEM;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    for (uint8_t source = 0; source < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; source++) {
        if (!peripheral_battery_seen[source]) {
            continue;
        }

        if (zmk_split_central_get_peripheral_battery_level(source, &percent) < 0) {
            continue;
        }

        if (percent > 100) {
            percent = 100;
        }

        if (!append_peripheral_battery(data, source, percent)) {
            LOG_ERR("behavior_battery_printer: buffer too small for all battery output");
            return -ENOMEM;
        }
    }
#endif

    return 0;
}

static int on_pressed(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_battery_printer_data *data = dev->data;
    const struct behavior_battery_printer_config *config = dev->config;

    if (data->key_pressed || data->current_idx) {
        /* typing in progress, ignore */
        return ZMK_BEHAVIOR_OPAQUE;
    }

    reset_typing_state(data);

    int err = config->print_all_batteries ? build_all_batteries_output(data)
                                          : build_single_battery_output(data, event);
    if (err < 0) {
        reset_typing_state(data);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    /* start typing */
    data->current_idx = 0;
    data->key_pressed = false;
    send_key(data);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_battery_printer_api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

#define BAT_INST(idx) \
    static struct behavior_battery_printer_data behavior_battery_printer_data_##idx; \
    static const struct behavior_battery_printer_config behavior_battery_printer_config_##idx = { \
        .print_all_batteries = DT_INST_PROP(idx, print_all_batteries), \
    }; \
    BEHAVIOR_DT_INST_DEFINE(idx, behavior_battery_printer_init, NULL, \
                            &behavior_battery_printer_data_##idx, \
                            &behavior_battery_printer_config_##idx, \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                            &behavior_battery_printer_api);

DT_INST_FOREACH_STATUS_OKAY(BAT_INST)


#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)

static int bapp_peripheral_batt_lvl_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (ev->source < ARRAY_SIZE(peripheral_battery_seen)) {
        peripheral_battery_seen[ev->source] = true;
    }
    LOG_DBG("batt_lvl_ev soruce: %d state_of_charge: %d", ev->source, ev->state_of_charge);
    return ZMK_EV_EVENT_BUBBLE;
};

ZMK_LISTENER(bapp_peripheral_batt_lvl_listener, bapp_peripheral_batt_lvl_listener);
ZMK_SUBSCRIPTION(bapp_peripheral_batt_lvl_listener, zmk_peripheral_battery_state_changed);

#endif
#endif


#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
