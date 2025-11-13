#ifndef SHARED_MEM_H
#define SHARED_MEM_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

typedef struct {
    int stream_flag;
} shared_mem_t;

extern shared_mem_t shared_mem;
extern SemaphoreHandle_t shared_mem_mutex;

void shared_mem_init();
void set_flag(int *flag, int val);
int get_flag(int *flag);
void message_handler(int flag);

#endif