#pragma once

#include <pthread.h>
#include <stdint.h>

typedef pthread_mutex_t rtos_osal_mutex_t;
typedef enum { RTOS_OSAL_SUCCESS, RTOS_OSAL_ERROR, RTOS_OSAL_TIMEOUT } rtos_osal_status_t;
#define RTOS_OSAL_NOT_RECURSIVE 0
#define RTOS_OSAL_NO_WAIT 0
#define RTOS_OSAL_WAIT_FOREVER UINT32_MAX

rtos_osal_status_t rtos_osal_mutex_create(rtos_osal_mutex_t *, char *, int);
rtos_osal_status_t rtos_osal_mutex_get(rtos_osal_mutex_t *, unsigned);
rtos_osal_status_t rtos_osal_mutex_put(rtos_osal_mutex_t *);
