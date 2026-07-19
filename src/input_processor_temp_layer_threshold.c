/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_temp_layer_threshold

#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/input_processor.h>
#include <zmk/behavior.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN

struct temp_layer_threshold_config {
    int16_t require_prior_idle_ms;
    int32_t reset_timeout_ms;
    int32_t threshold;
    const uint16_t *excluded_positions;
    size_t num_positions;
};

struct temp_layer_threshold_state {
    uint8_t layer;
    bool is_active;
    bool threshold_reached;
    int64_t last_tapped_timestamp;
    int64_t last_movement_timestamp;
    int32_t accumulated_movement;
};

struct rerouted_position_state {
    bool active;
    zmk_keymap_layer_id_t layer;
    struct zmk_behavior_binding binding;
};

struct temp_layer_threshold_data {
    struct k_mutex lock;
    struct temp_layer_threshold_state state;
    struct rerouted_position_state rerouted_positions[ZMK_KEYMAP_LEN];
};

static struct k_work_delayable layer_disable_works[MAX_LAYERS];

static bool position_is_excluded(const struct temp_layer_threshold_config *config,
                                 uint32_t position) {
    for (size_t i = 0; i < config->num_positions; i++) {
        if (config->excluded_positions[i] == position) {
            return true;
        }
    }
    return false;
}

static bool should_quick_tap(const struct temp_layer_threshold_config *config, int64_t last_tapped,
                             int64_t now) {
    return (last_tapped + config->require_prior_idle_ms) > now;
}

static int32_t add_movement_saturated(int32_t accumulated, int32_t movement) {
    int64_t magnitude = movement < 0 ? -(int64_t)movement : (int64_t)movement;

    if (magnitude >= INT32_MAX || accumulated >= INT32_MAX - magnitude) {
        return INT32_MAX;
    }

    return accumulated + (int32_t)magnitude;
}

static void update_layer_state(struct temp_layer_threshold_state *state, bool activate) {
    if (state->is_active == activate) {
        return;
    }

    state->is_active = activate;
    if (activate) {
        zmk_keymap_layer_activate(state->layer, false);
        LOG_DBG("Layer %d activated after movement threshold", state->layer);
    } else {
        zmk_keymap_layer_deactivate(state->layer, false);
        state->threshold_reached = false;
        state->accumulated_movement = 0;
        LOG_DBG("Layer %d deactivated", state->layer);
    }
}

struct layer_state_action {
    uint8_t layer;
    bool activate;
};

K_MSGQ_DEFINE(temp_layer_threshold_action_msgq, sizeof(struct layer_state_action),
              CONFIG_HITSUKI46_INPUT_PROCESSOR_TEMP_LAYER_THRESHOLD_MAX_ACTION_EVENTS, 4);

static void layer_action_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    const struct device *dev = DEVICE_DT_INST_GET(0);
    struct temp_layer_threshold_data *data = dev->data;
    struct layer_state_action action;

    if (k_mutex_lock(&data->lock, K_FOREVER) < 0) {
        return;
    }

    while (k_msgq_get(&temp_layer_threshold_action_msgq, &action, K_NO_WAIT) == 0) {
        if (action.activate || zmk_keymap_layer_active(action.layer)) {
            data->state.layer = action.layer;
            update_layer_state(&data->state, action.activate);
        }
    }

    k_mutex_unlock(&data->lock);
}

static K_WORK_DEFINE(layer_action_work, layer_action_work_cb);

static void layer_disable_callback(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    struct layer_state_action action = {
        .layer = ARRAY_INDEX(layer_disable_works, delayable),
        .activate = false,
    };

    if (k_msgq_put(&temp_layer_threshold_action_msgq, &action, K_NO_WAIT) == 0) {
        k_work_submit(&layer_action_work);
    }
}

static int handle_layer_state_changed(const struct device *dev, const zmk_event_t *eh) {
    ARG_UNUSED(eh);

    struct temp_layer_threshold_data *data = dev->data;

    if (k_mutex_lock(&data->lock, K_FOREVER) < 0) {
        return -EAGAIN;
    }

    if (!zmk_keymap_layer_active(data->state.layer)) {
        data->state.is_active = false;
        data->state.threshold_reached = false;
        data->state.accumulated_movement = 0;
        k_work_cancel_delayable(&layer_disable_works[data->state.layer]);
    }

    k_mutex_unlock(&data->lock);
    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_position_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_position_state_changed *event = as_zmk_position_state_changed(eh);
    struct temp_layer_threshold_data *data = dev->data;
    const struct temp_layer_threshold_config *config = dev->config;
    struct rerouted_position_state rerouted = {};

    if (event->position >= ZMK_KEYMAP_LEN) {
        LOG_ERR("Invalid key position: %u", event->position);
        return -EINVAL;
    }
    if (k_mutex_lock(&data->lock, K_FOREVER) < 0) {
        return -EAGAIN;
    }

    if (!event->state && data->rerouted_positions[event->position].active) {
        rerouted = data->rerouted_positions[event->position];
        data->rerouted_positions[event->position].active = false;
    } else if (event->state && data->state.is_active && config->num_positions > 0 &&
        !position_is_excluded(config, event->position)) {
        const zmk_keymap_layer_id_t default_layer = zmk_keymap_layer_default();
        const struct zmk_behavior_binding *binding =
            zmk_keymap_get_layer_binding_at_idx(default_layer, event->position);

        update_layer_state(&data->state, false);

        if (binding) {
            rerouted = (struct rerouted_position_state){
                .active = true,
                .layer = default_layer,
                .binding = *binding,
            };
            data->rerouted_positions[event->position] = rerouted;
        }
    }

    k_mutex_unlock(&data->lock);

    if (rerouted.active) {
        struct zmk_behavior_binding_event binding_event = {
            .layer = rerouted.layer,
            .position = event->position,
            .timestamp = event->timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            .source = event->source,
#endif
        };
        int ret = zmk_behavior_invoke_binding(&rerouted.binding, binding_event, event->state);

        if (ret < 0) {
            LOG_ERR("Failed to apply default-layer position %u (%d)", event->position, ret);

            if (event->state && k_mutex_lock(&data->lock, K_FOREVER) == 0) {
                data->rerouted_positions[event->position].active = false;
                k_mutex_unlock(&data->lock);
            }

            return ret;
        }

        return ZMK_EV_EVENT_HANDLED;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_keycode_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *event = as_zmk_keycode_state_changed(eh);
    struct temp_layer_threshold_data *data = dev->data;

    if (!event->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (k_mutex_lock(&data->lock, K_FOREVER) < 0) {
        return -EAGAIN;
    }

    data->state.last_tapped_timestamp = event->timestamp;
    k_mutex_unlock(&data->lock);
    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_state_changed_dispatcher(const struct device *dev, const zmk_event_t *eh) {
    if (as_zmk_layer_state_changed(eh)) {
        return handle_layer_state_changed(dev, eh);
    }
    if (as_zmk_position_state_changed(eh)) {
        return handle_position_state_changed(dev, eh);
    }
    if (as_zmk_keycode_state_changed(eh)) {
        return handle_keycode_state_changed(dev, eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

#define DISPATCH_EVENT(inst)                                                                       \
    do {                                                                                           \
        int err = handle_state_changed_dispatcher(DEVICE_DT_INST_GET(inst), eh);                  \
        if (err < 0) {                                                                             \
            return err;                                                                            \
        }                                                                                          \
    } while (false);

static int handle_event_dispatcher(const zmk_event_t *eh) {
    DT_INST_FOREACH_STATUS_OKAY(DISPATCH_EVENT)
    return 0;
}

static int temp_layer_threshold_handle_event(const struct device *dev, struct input_event *event,
                                             uint32_t layer, uint32_t timeout_ms,
                                             struct zmk_input_processor_state *state) {
    ARG_UNUSED(state);

    struct temp_layer_threshold_data *data = dev->data;
    const struct temp_layer_threshold_config *config = dev->config;
    int64_t now;
    bool should_activate = false;

    if (layer >= MAX_LAYERS) {
        LOG_ERR("Invalid layer index: %d", layer);
        return -EINVAL;
    }
    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    if (k_mutex_lock(&data->lock, K_FOREVER) < 0) {
        return -EAGAIN;
    }

    now = k_uptime_get();
    data->state.layer = layer;

    if ((now - data->state.last_movement_timestamp) > config->reset_timeout_ms) {
        data->state.accumulated_movement = 0;
        data->state.threshold_reached = false;
    }
    data->state.last_movement_timestamp = now;
    data->state.accumulated_movement =
        add_movement_saturated(data->state.accumulated_movement, event->value);

    if (!data->state.is_active &&
        !should_quick_tap(config, data->state.last_tapped_timestamp, now) &&
        (config->threshold == 0 || data->state.accumulated_movement >= config->threshold)) {
        data->state.threshold_reached = true;
        should_activate = true;
    }

    if (should_activate) {
        struct layer_state_action action = {.layer = layer, .activate = true};
        if (k_msgq_put(&temp_layer_threshold_action_msgq, &action, K_NO_WAIT) == 0) {
            k_work_submit(&layer_action_work);
        }
    }

    if (timeout_ms > 0 && (data->state.is_active || should_activate)) {
        k_work_reschedule(&layer_disable_works[layer], K_MSEC(timeout_ms));
    }

    k_mutex_unlock(&data->lock);
    return ZMK_INPUT_PROC_CONTINUE;
}

static int temp_layer_threshold_init(const struct device *dev) {
    struct temp_layer_threshold_data *data = dev->data;
    k_mutex_init(&data->lock);
    for (int i = 0; i < MAX_LAYERS; i++) {
        k_work_init_delayable(&layer_disable_works[i], layer_disable_callback);
    }
    return 0;
}

static const struct zmk_input_processor_driver_api temp_layer_threshold_driver_api = {
    .handle_event = temp_layer_threshold_handle_event,
};

ZMK_LISTENER(processor_temp_layer_threshold, handle_event_dispatcher);
ZMK_SUBSCRIPTION(processor_temp_layer_threshold, zmk_layer_state_changed);

#define NEEDS_POSITION_HANDLERS(n, ...) DT_INST_PROP_HAS_IDX(n, excluded_positions, 0)
#define NEEDS_KEYCODE_HANDLERS(n, ...) (DT_INST_PROP_OR(n, require_prior_idle_ms, 0) > 0)

#if DT_INST_FOREACH_STATUS_OKAY_VARGS(NEEDS_POSITION_HANDLERS, ||)
ZMK_SUBSCRIPTION(processor_temp_layer_threshold, zmk_position_state_changed);
#endif

#if DT_INST_FOREACH_STATUS_OKAY_VARGS(NEEDS_KEYCODE_HANDLERS, ||)
ZMK_SUBSCRIPTION(processor_temp_layer_threshold, zmk_keycode_state_changed);
#endif

#define TEMP_LAYER_THRESHOLD_INST(n)                                                               \
    static struct temp_layer_threshold_data processor_temp_layer_threshold_data_##n;              \
    static const uint16_t excluded_positions_##n[] = DT_INST_PROP(n, excluded_positions);          \
    static const struct temp_layer_threshold_config processor_temp_layer_threshold_config_##n = {  \
        .require_prior_idle_ms = DT_INST_PROP_OR(n, require_prior_idle_ms, 0),                     \
        .reset_timeout_ms = DT_INST_PROP_OR(n, reset_timeout_ms, 200),                             \
        .threshold = DT_INST_PROP_OR(n, threshold, 0),                                             \
        .excluded_positions = excluded_positions_##n,                                              \
        .num_positions = DT_INST_PROP_LEN(n, excluded_positions),                                  \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, temp_layer_threshold_init, NULL,                                      \
                          &processor_temp_layer_threshold_data_##n,                                \
                          &processor_temp_layer_threshold_config_##n, POST_KERNEL,                 \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &temp_layer_threshold_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TEMP_LAYER_THRESHOLD_INST)
