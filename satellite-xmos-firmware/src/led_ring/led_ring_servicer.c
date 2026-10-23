#include "debug_print.h"

#include <stdio.h>
#include <string.h>
#include <platform.h>
#include <xassert.h>

#include "platform/platform_conf.h"
#include "servicer.h"
#include "led_ring_servicer.h"

#include "led_ring_cmds.h"
#include "rtos_ws2812.h"
#include "audio/audio_ctrl.h"

#include "FreeRTOS.h"

#define LED_RING_BYTES_PER_LED          3
#define LED_RING_FRAME_BYTES            (LED_RING_BYTES_PER_LED * LED_RING_NUM_LEDS)
#define LED_RING_DOA_POLL_MS            50

static control_cmd_info_t led_ring_servicer_resid_cmd_map[] = {
    { LED_RING_SERVICER_CMD_WRITE_RAW,      LED_RING_FRAME_BYTES,       sizeof(uint8_t), CMD_WRITE_ONLY },
    { LED_RING_SERVICER_CMD_DOA_INDICATOR,  1 + LED_RING_BYTES_PER_LED, sizeof(uint8_t), CMD_WRITE_ONLY },
};

/*
 * The host owns the ring through WRITE_RAW. With the DOA indicator enabled, the
 * LED pointing at the talker is drawn over the host's last frame while a DOA
 * result is valid. Nothing is drawn until the host sent its first frame, so
 * enabling the indicator early does not blank the ring. Only the servicer task
 * touches this state.
 */
static uint8_t host_frame[LED_RING_FRAME_BYTES];
static int host_frame_received;
static uint8_t doa_colour[LED_RING_BYTES_PER_LED];
static int doa_enabled;
static int doa_led = -1;    /* LED currently showing the indicator, -1 for none */

static int led_for_azimuth(unsigned azimuth_deg)
{
    int steps = (int)((azimuth_deg * LED_RING_NUM_LEDS + 180) / 360) % LED_RING_NUM_LEDS;
#if !appconfLED_RING_DOA_CLOCKWISE
    steps = (LED_RING_NUM_LEDS - steps) % LED_RING_NUM_LEDS;
#endif
    return (appconfLED_RING_DOA_LED_OFFSET + steps) % LED_RING_NUM_LEDS;
}

static void led_ring_refresh(rtos_ws2812_t *ws2812_ctx)
{
    if (!host_frame_received) {
        return;
    }
    uint8_t frame[LED_RING_FRAME_BYTES];
    memcpy(frame, host_frame, sizeof(frame));
    if (doa_led >= 0) {
        memcpy(&frame[doa_led * LED_RING_BYTES_PER_LED], doa_colour, LED_RING_BYTES_PER_LED);
    }
    rtos_ws2812_write(ws2812_ctx, frame);
}

/* LED that should show the indicator now, -1 for none */
static int led_ring_doa_target(void)
{
    if (doa_enabled) {
        const doa_result_t doa = audio_ctrl_get_doa();
        if (doa.valid) {
            return led_for_azimuth(doa.azimuth_deg);
        }
    }
    return -1;
}

static void led_ring_doa_poll(rtos_ws2812_t *ws2812_ctx)
{
    const int led = led_ring_doa_target();
    if (led != doa_led) {
        doa_led = led;
        led_ring_refresh(ws2812_ctx);
    }
}

//-----------------Servicer read write callback functions-----------------------//
DEVICE_CONTROL_CALLBACK_ATTR
static control_ret_t led_ring_servicer_read_cmd(control_resid_t resid, control_cmd_t cmd, uint8_t *payload, size_t payload_len, void *app_data)
{
    // no read command implemented yet
    payload[0] = CONTROL_BAD_COMMAND;
    return CONTROL_BAD_COMMAND;
}

DEVICE_CONTROL_CALLBACK_ATTR
static control_ret_t led_ring_servicer_write_cmd(control_resid_t resid, control_cmd_t cmd, const uint8_t *payload, size_t payload_len, void *app_data)
{
    control_ret_t ret = CONTROL_SUCCESS;
    led_ring_servicer_ctx_t *led_servicer_ctx = (led_ring_servicer_ctx_t*) app_data;
    servicer_t *servicer = led_servicer_ctx->servicer; 
    rtos_ws2812_t *ws2812_ctx = led_servicer_ctx->ws2812_ctx;
    
    debug_printf("LED-Ring servicer on tile %d received WRITE command %02x for resid %02x\n\t", THIS_XCORE_TILE, cmd, resid);
    debug_printf("The command has %d bytes\n\t", payload_len);

    control_resource_info_t *current_res_info = get_res_info(resid, servicer);
    xassert(current_res_info != NULL);
    control_cmd_info_t *current_cmd_info;
    ret = validate_cmd(&current_cmd_info, current_res_info, cmd, payload, payload_len);
    if(ret != CONTROL_SUCCESS)
    {
        debug_printf("Validation of command failed! error: %d\n", ret);
        return ret;
    }
    
    //handle command
    uint8_t cmd_id = CONTROL_CMD_CLEAR_READ(cmd);
    debug_printf("led_ring_servicer_write_cmd cmd_id %d.\n", cmd_id);

    switch (cmd_id)
    {
    
    case LED_RING_SERVICER_CMD_WRITE_RAW:
    {
        debug_printf("LED_RING_SERVICER_CMD_WRITE_RAW\n");
        debug_printf("%d %d %d\n", payload[0], payload[1], payload[2]);
        memcpy(host_frame, payload, LED_RING_FRAME_BYTES);
        host_frame_received = 1;
        led_ring_refresh(ws2812_ctx);
        break;
    }

    case LED_RING_SERVICER_CMD_DOA_INDICATOR:
    {
        doa_enabled = payload[0] != 0;
        const int led = led_ring_doa_target();
        const int changed = led != doa_led ||
                            (led >= 0 && memcmp(doa_colour, &payload[1], LED_RING_BYTES_PER_LED) != 0);
        memcpy(doa_colour, &payload[1], LED_RING_BYTES_PER_LED);
        doa_led = led;
        if (changed) {
            led_ring_refresh(ws2812_ctx);
        }
        break;
    }
    
    default:
        debug_printf("LED_RING_CONTROLLER_SERVICER UNHANDLED COMMAND!!!\n");
        ret = CONTROL_BAD_COMMAND;
        break;
    }

    return ret;
}



void led_ring_servicer_init(servicer_t *servicer)
{
    // Servicer resource info
    static control_resource_info_t led_ring_res_info[NUM_RESOURCES_LED_RING];

    memset(servicer, 0, sizeof(servicer_t));
    servicer->id = LED_RING_SERVICER_RESID;
    servicer->start_io = 0;
    servicer->num_resources = NUM_RESOURCES_LED_RING;

    servicer->res_info = &led_ring_res_info[0];
    // Servicer resource
    servicer->res_info[0].resource = LED_RING_SERVICER_RESID;
    servicer->res_info[0].command_map.num_commands = NUM_LED_RING_SERVICER_CMDS;
    servicer->res_info[0].command_map.commands = led_ring_servicer_resid_cmd_map;
}

void led_ring_servicer(void *args) {
    device_control_servicer_t servicer_ctx;

    servicer_register_ctx_t *servicer_reg_ctx = (servicer_register_ctx_t*)args;
    
    servicer_t *servicer = servicer_reg_ctx->servicer;
    rtos_ws2812_t *ws2812_ctx = (rtos_ws2812_t *) servicer_reg_ctx->app_data;
    
    xassert(servicer != NULL);
    xassert(ws2812_ctx != NULL);
    
    control_resid_t *resources = (control_resid_t*)pvPortMalloc(servicer->num_resources * sizeof(control_resid_t));
    for(int i=0; i<servicer->num_resources; i++)
    {
        resources[i] = servicer->res_info[i].resource;
    }

    control_ret_t dc_ret;
    debug_printf("Calling device_control_servicer_register(), servicer ID %d, on tile %d, core %d.\n", servicer->id, THIS_XCORE_TILE, rtos_core_id_get());

    dc_ret = device_control_servicer_register(&servicer_ctx,
                                            servicer_reg_ctx->device_control_ctx,
                                            1,
                                            resources, servicer->num_resources);
    debug_printf("Out of device_control_servicer_register(), servicer ID %d, on tile %d. servicer_ctx address = 0x%x\n", servicer->id, THIS_XCORE_TILE, &servicer_ctx);

    vPortFree(resources);

    led_ring_servicer_ctx_t led_servicer_ctx = {
        servicer,
        ws2812_ctx
    };

    for(;;){
        /*
         * On timeout this returns CONTROL_ERROR without logging or dequeuing
         * anything, so commands that arrive later are still handled.
         */
        (void) device_control_servicer_cmd_recv(&servicer_ctx, led_ring_servicer_read_cmd, led_ring_servicer_write_cmd, &led_servicer_ctx, RTOS_OSAL_WAIT_MS(LED_RING_DOA_POLL_MS));
        led_ring_doa_poll(ws2812_ctx);
    }
}

