#pragma once
#include <assert.h>
#include <stddef.h>
#define portMAX_DELAY 0
#define ESP_ERR_NO_MEM 1
#define ESP_OK 0
#define ESP_ERROR_CHECK(value) assert((value) == ESP_OK)
size_t strlcpy(char *destination, const char *source, size_t capacity);
