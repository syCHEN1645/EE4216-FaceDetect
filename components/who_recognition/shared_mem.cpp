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
    xSemaphoreTake(shared_mem_mutex, pdMS_TO_TICKS(500));
    *flag = val;
    xSemaphoreGive(shared_mem_mutex);
}

int get_flag(int *flag) {
    int val;
    xSemaphoreTake(shared_mem_mutex, pdMS_TO_TICKS(500));
    val = *flag;
    xSemaphoreGive(shared_mem_mutex);
    return val;
}

void message_handler(int flag)
{
    /*
    0: default
    1: make a recognition while streaming
    2: send a picture and stop streaming
    */
    switch (flag)
    {
    case 3:
        // motion sensor is triggered
        set_flag(&shared_mem.stream_flag, 3);
        break;
    case 2:
        // face is known, take photo
        set_flag(&shared_mem.stream_flag, 2);
        break;
    case 1:
        // face is unknown, take photo and stream
        set_flag(&shared_mem.stream_flag, 1);
        break;
    case 0:
        set_flag(&shared_mem.stream_flag, 0);
        break;
    default:
        ESP_LOGI("Shared mem", "Unknown flag %d received", flag);
        break;
    }
}