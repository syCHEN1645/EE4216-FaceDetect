#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <nvs_flash.h>

#include "frame_cap_pipeline.hpp"
#include "who_recognition_app_lcd.hpp"
#include "who_recognition_app_term.hpp"
#include "who_spiflash_fatfs.hpp"
#include "web_stream.cpp"

using namespace who::frame_cap;
using namespace who::app;

// WiFi credentials
#define WIFI_SSID "abc"
#define WIFI_PASSWORD "33333333"
// WiFi connection settings
#define MAX_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_CONNECT_FAIL_BIT BIT1

static EventGroupHandle_t wifi_event_group;
const char* TAG_WIFI = "WiFi";
int s_retry_num = 0;

// http server
static httpd_handle_t server = NULL;

static void event_handler(
    void* arg, 
    esp_event_base_t event_base,
    int32_t event_id, 
    void* event_data)
{
    if (event_base == WIFI_EVENT) { 
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            // Try connect wifi
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            // Disconnected event, retry connection
            if (s_retry_num < MAX_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG_WIFI, "Disconnect event, retry to connect to the AP");
            } else {
                xEventGroupSetBits(wifi_event_group, WIFI_CONNECT_FAIL_BIT);
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            // Connected
            ESP_LOGI(TAG_WIFI, "Successfully connected to %s", WIFI_SSID);
            break;
        default:
            break;
        }
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // IP addr got successfully, meaning connection success
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        // Special formatting for IPv4
        ESP_LOGI(TAG_WIFI, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        // Signal successful connection
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        if (!server) {
            ESP_LOGI(TAG_WIFI, "Starting http server now at " IPSTR, IP2STR(&event->ip_info.ip));
            server = init_http();
        }
    }
}

void init_wifi(void)
{
    wifi_event_group = xEventGroupCreate();
    
    // Some initial error checks
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop & network interface
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // Set wifi configurations and call init functions
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Release (no need, let wifi run forever)
    // ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    // ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    // vEventGroupDelete(wifi_event_group);
}

extern "C" void app_main(void)
{
    vTaskPrioritySet(xTaskGetCurrentTaskHandle(), 5);
#if CONFIG_DB_FATFS_FLASH
    ESP_ERROR_CHECK(fatfs_flash_mount());
#elif CONFIG_DB_SPIFFS
    ESP_ERROR_CHECK(bsp_spiffs_mount());
#endif
#if CONFIG_DB_FATFS_SDCARD || CONFIG_HUMAN_FACE_DETECT_MODEL_IN_SDCARD || CONFIG_HUMAN_FACE_FEAT_MODEL_IN_SDCARD
    ESP_ERROR_CHECK(bsp_sdcard_mount());
#endif

// close led
#ifdef BSP_BOARD_ESP32_S3_EYE
    ESP_ERROR_CHECK(bsp_leds_init());
    ESP_ERROR_CHECK(bsp_led_set(BSP_LED_GREEN, false));
#endif

#if CONFIG_IDF_TARGET_ESP32S3
    auto frame_cap = get_dvp_frame_cap_pipeline();
#elif CONFIG_IDF_TARGET_ESP32P4
    auto frame_cap = get_mipi_csi_frame_cap_pipeline();
    // auto frame_cap = get_uvc_frame_cap_pipeline();
#endif
    init_wifi();
    // hold until connection success
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_CONNECT_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);
    
    auto recognition_app = new WhoRecognitionAppTerm(frame_cap);
    recognition_app->run();
}
