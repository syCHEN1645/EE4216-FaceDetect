#include "shared_mem.hpp"

shared_mem_t shared_mem;
SemaphoreHandle_t shared_mem_mutex;

void shared_mem_init() {
    shared_mem_mutex = xSemaphoreCreateMutex();
    shared_mem.stream_flag = 0;
}

void set_flag(int *flag, int val) {
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