#include "debug_print.h"

#include <string.h>
#include <platform.h>
#include <xassert.h>

#include "platform/platform_conf.h"
#include "servicer.h"
#include "stats_servicer.h"
#include "stats_cmds.h"
#include "audio/stage_stats.h"

#include "FreeRTOS.h"

static control_cmd_info_t stats_servicer_resid_cmd_map[] = {
    { STATS_SERVICER_CMD_STAGE_TIMES, 2 * STAGE_STATS_COUNT, sizeof(uint16_t), CMD_READ_ONLY },
    { STATS_SERVICER_CMD_STACK_FREE,  STAGE_STATS_COUNT,     sizeof(uint16_t), CMD_READ_ONLY },
    { STATS_SERVICER_CMD_HEAP_FREE,   4,                     sizeof(uint32_t), CMD_READ_ONLY },
};

//-----------------Servicer read write callback functions-----------------------//
DEVICE_CONTROL_CALLBACK_ATTR
static control_ret_t stats_servicer_read_cmd(control_resid_t resid, control_cmd_t cmd, uint8_t *payload, size_t payload_len, void *app_data)
{
    servicer_t *servicer = (servicer_t*)app_data;

    // For read commands, payload[0] is reserved for status. So payload_len is one more than the payload_len stored in the resource command map
    payload_len -= 1;
    uint8_t *payload_ptr = &payload[1];

    control_resource_info_t *current_res_info = get_res_info(resid, servicer);
    xassert(current_res_info != NULL);
    control_cmd_info_t *current_cmd_info;
    control_ret_t ret = validate_cmd(&current_cmd_info, current_res_info, cmd, payload_ptr, payload_len);
    if(ret != CONTROL_SUCCESS)
    {
        payload[0] = ret;
        return ret;
    }

    /* xcore is little-endian, so the values are copied as they are */
    switch (CONTROL_CMD_CLEAR_READ(cmd))
    {
    case STATS_SERVICER_CMD_STAGE_TIMES:
    {
        uint16_t times[2][STAGE_STATS_COUNT];
        stage_stats_read_times_us(times[0], times[1]);
        memcpy(payload_ptr, times, sizeof(times));
        break;
    }

    case STATS_SERVICER_CMD_STACK_FREE:
    {
        uint16_t free_words[STAGE_STATS_COUNT];
        stage_stats_read_stack_free(free_words);
        memcpy(payload_ptr, free_words, sizeof(free_words));
        break;
    }

    case STATS_SERVICER_CMD_HEAP_FREE:
    {
        uint32_t heap[4];
        stage_stats_read_heap(heap);
        memcpy(payload_ptr, heap, sizeof(heap));
        break;
    }

    default:
        ret = CONTROL_BAD_COMMAND;
        break;
    }

    payload[0] = ret;
    return ret;
}

DEVICE_CONTROL_CALLBACK_ATTR
static control_ret_t stats_servicer_write_cmd(control_resid_t resid, control_cmd_t cmd, const uint8_t *payload, size_t payload_len, void *app_data)
{
    /* every command is read only */
    return CONTROL_BAD_COMMAND;
}


void stats_servicer_init(servicer_t *servicer)
{
    static control_resource_info_t stats_res_info[NUM_RESOURCES_STATS_SERVICER];

    memset(servicer, 0, sizeof(servicer_t));
    servicer->id = STATS_SERVICER_RESID;
    servicer->start_io = 0;
    servicer->num_resources = NUM_RESOURCES_STATS_SERVICER;

    servicer->res_info = &stats_res_info[0];
    servicer->res_info[0].resource = STATS_SERVICER_RESID;
    servicer->res_info[0].command_map.num_commands = NUM_STATS_SERVICER_CMDS;
    servicer->res_info[0].command_map.commands = stats_servicer_resid_cmd_map;
}

void stats_servicer(void *args)
{
    device_control_servicer_t servicer_ctx;

    servicer_register_ctx_t *servicer_reg_ctx = (servicer_register_ctx_t*)args;
    servicer_t *servicer = servicer_reg_ctx->servicer;
    xassert(servicer != NULL);

    control_resid_t resources[NUM_RESOURCES_STATS_SERVICER];
    for(int i=0; i<servicer->num_resources; i++)
    {
        resources[i] = servicer->res_info[i].resource;
    }

    control_ret_t dc_ret = device_control_servicer_register(&servicer_ctx,
                                                            servicer_reg_ctx->device_control_ctx,
                                                            servicer_reg_ctx->device_control_ctx_count,
                                                            resources, servicer->num_resources);
    xassert(dc_ret == CONTROL_SUCCESS);

    for(;;){
        device_control_servicer_cmd_recv(&servicer_ctx, stats_servicer_read_cmd, stats_servicer_write_cmd, servicer, RTOS_OSAL_WAIT_FOREVER);
    }
}
