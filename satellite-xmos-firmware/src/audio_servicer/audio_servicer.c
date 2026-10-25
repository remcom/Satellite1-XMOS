#include "debug_print.h"

#include <string.h>
#include <platform.h>
#include <xassert.h>

#include "platform/platform_conf.h"
#include "servicer.h"
#include "audio_servicer.h"
#include "audio_cmds.h"
#include "audio/audio_ctrl.h"

#include "FreeRTOS.h"

/* DOA update count at the previous DOA read; only the servicer task touches it */
static uint32_t doa_read_count;

static control_cmd_info_t audio_servicer_resid_cmd_map[] = {
    { AUDIO_SERVICER_CMD_MIC_GAIN, 1, sizeof(uint8_t), CMD_READ_WRITE },
    { AUDIO_SERVICER_CMD_MIC_MODE, 1, sizeof(uint8_t), CMD_READ_WRITE },
    { AUDIO_SERVICER_CMD_DOA,      4, sizeof(uint8_t), CMD_READ_ONLY  },
    { AUDIO_SERVICER_CMD_MIC_PAIR, 2, sizeof(uint8_t), CMD_READ_ONLY  },
    { AUDIO_SERVICER_CMD_BEAM,     2, sizeof(uint8_t), CMD_READ_ONLY  },
};

//-----------------Servicer read write callback functions-----------------------//
DEVICE_CONTROL_CALLBACK_ATTR
static control_ret_t audio_servicer_read_cmd(control_resid_t resid, control_cmd_t cmd, uint8_t *payload, size_t payload_len, void *app_data)
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

    switch (CONTROL_CMD_CLEAR_READ(cmd))
    {
    case AUDIO_SERVICER_CMD_MIC_GAIN:
        payload_ptr[0] = (uint8_t) audio_ctrl_get_mic_gain_db();
        break;

    case AUDIO_SERVICER_CMD_MIC_MODE:
        payload_ptr[0] = (uint8_t) audio_ctrl_get_mic_mode();
        break;

    case AUDIO_SERVICER_CMD_DOA:
    {
        const uint32_t count = audio_ctrl_get_doa_update_count();
        const doa_result_t doa = audio_ctrl_get_doa();
        const int updated = count != doa_read_count;
        doa_read_count = count;
        payload_ptr[0] = (uint8_t)(doa.azimuth_deg & 0xFF);
        payload_ptr[1] = (uint8_t)(doa.azimuth_deg >> 8);
        payload_ptr[2] = doa.confidence;
        payload_ptr[3] = (doa.valid ? AUDIO_SERVICER_DOA_FLAG_VALID : 0) |
                         (updated ? AUDIO_SERVICER_DOA_FLAG_UPDATED : 0);
        break;
    }

    case AUDIO_SERVICER_CMD_MIC_PAIR:
        audio_ctrl_get_mic_masks(&payload_ptr[0], &payload_ptr[1]);
        break;

    case AUDIO_SERVICER_CMD_BEAM:
    {
        const uint16_t azimuth = audio_ctrl_get_beam_azimuth();
        payload_ptr[0] = (uint8_t)(azimuth & 0xFF);
        payload_ptr[1] = (uint8_t)(azimuth >> 8);
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
static control_ret_t audio_servicer_write_cmd(control_resid_t resid, control_cmd_t cmd, const uint8_t *payload, size_t payload_len, void *app_data)
{
    servicer_t *servicer = (servicer_t*)app_data;

    control_resource_info_t *current_res_info = get_res_info(resid, servicer);
    xassert(current_res_info != NULL);
    control_cmd_info_t *current_cmd_info;
    control_ret_t ret = validate_cmd(&current_cmd_info, current_res_info, cmd, payload, payload_len);
    if(ret != CONTROL_SUCCESS)
    {
        return ret;
    }
    if(current_cmd_info->cmd_rw_type == CMD_READ_ONLY)
    {
        return CONTROL_BAD_COMMAND;
    }

    switch (CONTROL_CMD_CLEAR_READ(cmd))
    {
    case AUDIO_SERVICER_CMD_MIC_GAIN:
        if (audio_ctrl_set_mic_gain_db(payload[0]) != 0) {
            ret = SERVICER_WRONG_PAYLOAD;
        }
        break;

    case AUDIO_SERVICER_CMD_MIC_MODE:
        if (audio_ctrl_set_mic_mode(payload[0]) != 0) {
            ret = SERVICER_WRONG_PAYLOAD;
        }
        break;

    default:
        ret = CONTROL_BAD_COMMAND;
        break;
    }

    return ret;
}


void audio_servicer_init(servicer_t *servicer)
{
    static control_resource_info_t audio_res_info[NUM_RESOURCES_AUDIO_SERVICER];

    memset(servicer, 0, sizeof(servicer_t));
    servicer->id = AUDIO_SERVICER_RESID;
    servicer->start_io = 0;
    servicer->num_resources = NUM_RESOURCES_AUDIO_SERVICER;

    servicer->res_info = &audio_res_info[0];
    servicer->res_info[0].resource = AUDIO_SERVICER_RESID;
    servicer->res_info[0].command_map.num_commands = NUM_AUDIO_SERVICER_CMDS;
    servicer->res_info[0].command_map.commands = audio_servicer_resid_cmd_map;
}

void audio_servicer(void *args)
{
    device_control_servicer_t servicer_ctx;

    servicer_register_ctx_t *servicer_reg_ctx = (servicer_register_ctx_t*)args;
    servicer_t *servicer = servicer_reg_ctx->servicer;
    xassert(servicer != NULL);

    control_resid_t resources[NUM_RESOURCES_AUDIO_SERVICER];
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
        device_control_servicer_cmd_recv(&servicer_ctx, audio_servicer_read_cmd, audio_servicer_write_cmd, servicer, RTOS_OSAL_WAIT_FOREVER);
    }
}
