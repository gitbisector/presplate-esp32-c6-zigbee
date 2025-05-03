/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */
#include "esp_check.h"
#include "hal/gpio_types.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zb_pplate.h"
#include "switch_driver.h"
#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_sleep.h"
#endif
#include "driver/rtc_io.h"
#include "driver/gpio.h"

/**
 * @note Make sure set idf.py menuconfig in zigbee component as zigbee end device!
*/
#if !defined ZB_ED_ROLE
#error Define ZB_ED_ROLE in idf.py menuconfig to compile light (End Device) source code.
#endif

static const char *TAG = "ESP_ZB_SLEEP";

static switch_func_pair_t button_func_pair[] = {
    {GPIO_INPUT_IO_TOGGLE_SWITCH, SWITCH_ONOFF_TOGGLE_CONTROL}
};

static volatile bool pending_deep_sleep = false;

static void go_to_deep_sleep(void)
{
    ESP_LOGI(TAG, "Now entering deep sleep.");
    esp_deep_sleep_start();
}

static void attr_onoff(uint8_t v) 
{
    uint8_t onoff_value = v; // 1 = ON, 0 = OFF
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(
        10,                // e.g., 10 or your defined endpoint
        ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,     // 0x0006
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,   // Server role (the switch provides the attribute)
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, // 0x0000 (On/Off attribute)
        &onoff_value,                     // Pointer to value (1 = on, 0 = off)
        false                             // Do not send a report automatically
    );
    esp_zb_lock_release();
}

static void zb_buttons_handler(switch_func_pair_t* button_func_pair)
{
    if (button_func_pair->func == SWITCH_ONOFF_TOGGLE_CONTROL) {
        attr_onoff(1);
        ESP_EARLY_LOGI(TAG, "Change 'report attributes'");
    }
}

static esp_err_t deferred_driver_init(void)
{
    static bool is_inited = false;
    if (!is_inited) {
        ESP_RETURN_ON_FALSE(switch_driver_init(button_func_pair, PAIR_SIZE(button_func_pair), zb_buttons_handler),
                            ESP_FAIL, TAG, "Failed to initialize switch driver");
        /* Configure RTC IO wake up:
        The configuration mode depends on your hardware design.
        Since the BOOT button is connected to a pull-up resistor, the wake-up mode is configured as LOW.
        */
        //ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup(1ULL << CONFIG_GPIO_INPUT_IO_WAKEUP, ESP_EXT1_WAKEUP_ANY_LOW));

        is_inited = true;
    }
    return is_inited ? ESP_OK : ESP_FAIL;
}

/********************* Define functions **************************/
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG, "Failed to start Zigbee bdb commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p       = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : " non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted");
                esp_zb_scheduler_alarm((esp_zb_callback_t)attr_onoff, 1, 100);
            }
        } else {
            ESP_LOGW(TAG, "%s failed with status: %s, retrying", esp_zb_zdo_signal_to_string(sig_type),
                     esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
        } else {
            ESP_LOGI(TAG, "Network steering was not successful (status: %d)", err_status);
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
      case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP:
        ESP_LOGI(TAG, "Zigbee can sleep");
        esp_zb_sleep_now();
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(err_status));
        break;
    }
}

static esp_err_t esp_zb_power_save_init(void)
{
    esp_err_t rc = ESP_OK;
#ifdef CONFIG_PM_ENABLE
    int cur_cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    esp_pm_config_t pm_config = {
        .max_freq_mhz = cur_cpu_freq_mhz,
        .min_freq_mhz = cur_cpu_freq_mhz,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
        .light_sleep_enable = true
#endif
    };
    rc = esp_pm_configure(&pm_config);
#endif
    return rc;
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    bool light_state = 0;

    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG, "Received message: error status(%d)",
                        message->info.status);
    ESP_LOGI(TAG, "Received message: endpoint(%d), cluster(0x%x), attribute(0x%x), data size(%d)", message->info.dst_endpoint, message->info.cluster,
             message->attribute.id, message->attribute.data.size);
    if (message->info.dst_endpoint == HA_ESP_LIGHT_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
                light_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : light_state;
                ESP_LOGI(TAG, "Light sets to %s", light_state ? "On" : "Off");
                if (!light_state) {
                    ESP_LOGI(TAG, "Received OFF attribute, will sleep after response.");
                    pending_deep_sleep = true;
                }
            }
        }
    }
    return ret;
}

// Helper to print callback_id as string
static const char* zb_action_name(esp_zb_core_action_callback_id_t callback_id) {
    switch (callback_id) {
        case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: return "SET_ATTR_VALUE";
        case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID: return "DEFAULT_RESP";
        // Add more known cases here as needed
        default: return "UNKNOWN";
    }
}

static void log_message_hex(const void *msg, size_t len) {
    if (!msg) {
        ESP_LOGW(TAG, "  message pointer is NULL");
        return;
    }
    const uint8_t *bytes = (const uint8_t *)msg;
    char buf[3 * 8 + 1] = {0}; // up to 8 bytes
    size_t i;
    for (i = 0; i < len && i < 8; ++i) {
        sprintf(buf + i * 3, "%02X ", bytes[i]);
    }
    ESP_LOGW(TAG, "  message first %d bytes: %s", (int)i, buf);
}

static bool waiting_for_default_resp = false;
#define DEFAULT_RESP_TIMEOUT_MS 30000

static void default_resp_timeout_cb(uint8_t param)
{
    (void)param;
    if (waiting_for_default_resp) {
        ESP_LOGW(TAG, "Default Response not received within timeout, entering deep sleep.");
        waiting_for_default_resp = false;
        go_to_deep_sleep();
    }
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        if (pending_deep_sleep) {
            ESP_LOGI(TAG, "Attribute response sent, waiting for Default Response before deep sleep.");
            pending_deep_sleep = false;
            waiting_for_default_resp = true;
            // Start 30s timeout
            esp_zb_scheduler_alarm(default_resp_timeout_cb, 0, DEFAULT_RESP_TIMEOUT_MS);
        }
        break;
    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
        ESP_LOGI(TAG, "Received Default Response (0x1005)");
        if (waiting_for_default_resp) {
            ESP_LOGI(TAG, "Default Response received, entering deep sleep now.");
            waiting_for_default_resp = false;
            esp_zb_scheduler_alarm_cancel(default_resp_timeout_cb, 0);
            go_to_deep_sleep();
        }
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) (%s) callback", callback_id, zb_action_name(callback_id));
        ESP_LOGW(TAG, "  message pointer: %p", message);
        log_message_hex(message, 8);
        break;
    }
    return ret;
}

static void esp_zb_task(void *pvParameters)
{
    /* initialize Zigbee stack with Zigbee end-device config */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    /* Enable zigbee light sleep */
    //esp_zb_sleep_enable(true);
    esp_zb_init(&zb_nwk_cfg);
    /* set the on-off light device config */
    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_ep_list_t *esp_zb_on_off_light_ep = esp_zb_on_off_light_ep_create(HA_ESP_LIGHT_ENDPOINT, &light_cfg);
    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };

    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_on_off_light_ep, HA_ESP_LIGHT_ENDPOINT, &info);
    esp_zb_device_register(esp_zb_on_off_light_ep);
    esp_zb_core_action_handler_register(zb_action_handler);
//    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    esp_zb_set_primary_network_channel_set(1<<20);
    esp_zb_secur_link_key_exchange_required_set(false);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };

    // Enable internal antenna by pulling down GPIO3
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 3),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(3, 0);


    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    /* esp zigbee light sleep initialization*/
    //ESP_ERROR_CHECK(esp_zb_power_save_init());
    /* load Zigbee platform config to initialization */
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
