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
    //xSemaphoreTake(shared_mem_mutex, pdMS_TO_TICKS(20));
    *flag = val;
    //xSemaphoreGive(shared_mem_mutex);
}

int get_flag(int *flag) {
    int val;
    //xSemaphoreTake(shared_mem_mutex, pdMS_TO_TICKS(500));
    val = *flag;
    //xSemaphoreGive(shared_mem_mutex);
    return val;
}
