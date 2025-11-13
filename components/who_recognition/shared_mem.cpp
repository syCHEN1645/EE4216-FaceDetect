#include "shared_mem.hpp"
#include "esp_log.h"

shared_mem_t shared_mem;
SemaphoreHandle_t shared_mem_mutex;

void shared_mem_init() {
    shared_mem_mutex = xSemaphoreCreateMutex();
    shared_mem.stream_flag = 0;
}

void set_flag(int *flag, int val) {
    if (shared_mem_mutex == NULL) {
        ESP_LOGE("Shared mem", "Mutex is NULL in set_flag!");
        return;
    }
    if (flag == NULL) {
        ESP_LOGE("Shared mem", "Flag pointer is NULL!");
        return;
    }
    ESP_LOGI("Shared mem", "Setting flag to %d", val);
    xSemaphoreTake(shared_mem_mutex, portMAX_DELAY);
    *flag = val;
    xSemaphoreGive(shared_mem_mutex);
}

int get_flag(int *flag) {
    int val;
    xSemaphoreTake(shared_mem_mutex, portMAX_DELAY);
    val = *flag;
    xSemaphoreGive(shared_mem_mutex);
    return val;
}

void message_handler(int flag)
{
    /*
    0: stop streaming, dont do anything
    1: make a recognition while streaming
    2: send a picture and stop streaming
    3: keep streaming
    */
    switch (flag)
    {
    case 3:
        set_flag(&shared_mem.stream_flag, 3);
        ESP_LOGI("Shared mem", "An unknown face detected, streaming");
        break;
    case 2:
        set_flag(&shared_mem.stream_flag, 2);
        ESP_LOGI("Shared mem", "A known face detected, stop streaming");
        break;
    case 1:
        // motion is detected, do a scan
        // xEventGroupSetBits(m_event_group, RECOGNIZE);
        // stream video
        set_flag(&shared_mem.stream_flag, 1);
        ESP_LOGI("Shared mem", "Motion detected, attempt to recognize");
        break;
    case 0:
        set_flag(&shared_mem.stream_flag, 0);
        // stop streaming and standby
        ESP_LOGI("Shared mem", "Streaming stops, standby");
        break;
    default:
        ESP_LOGI("Shared mem", "Unknown flag %d received", flag);
        break;
    }
}