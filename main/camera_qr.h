#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called from LVGL task context when a Matter QR payload is detected.
// The payload string ("MT:...") is valid only for the duration of the callback.
// Do not call camera_qr_stop() from within this callback.
typedef void (*camera_qr_result_cb_t)(const char *payload);

// Start the camera QR scanner.
// preview_canvas must be an lv_canvas widget sized CAM_QR_DISP_W x CAM_QR_DISP_H.
// Called from LVGL task context.
#define CAM_QR_DISP_W 800
#define CAM_QR_DISP_H 480

esp_err_t camera_qr_start(camera_qr_result_cb_t on_result, lv_obj_t *preview_canvas);

// Stop the camera scanner. Safe to call even if never started.
// The camera hardware cleanup happens asynchronously in the frame task.
// Called from LVGL task context.
void camera_qr_stop(void);

#ifdef __cplusplus
}
#endif
