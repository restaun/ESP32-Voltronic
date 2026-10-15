#pragma once
typedef struct { int held; } *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mutex, int timeout);
int xSemaphoreGive(SemaphoreHandle_t mutex);
