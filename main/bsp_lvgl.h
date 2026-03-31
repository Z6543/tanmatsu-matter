#include "display/lv_display.h"
#include "esp_lcd_types.h"
#include "hal/lcd_types.h"
#include "misc/lv_types.h"

#define DEVKIT

// Custom key codes for function keys not in lv_key_t
#define BSP_KEY_F3  0xF3

/// @brief Lock LVGL
void lvgl_lock();
/// @brief Unlock LVGL
void lvgl_unlock();

/// @brief Get LVGL display pointer
lv_display_t* lvgl_get_display(void);

/// @brief Initialise LVGL
void lvgl_init(int32_t hres, int32_t vres, lcd_color_rgb_pixel_format_t colour_fmt,
               esp_lcd_panel_handle_t lcd_panel_handle, esp_lcd_panel_io_handle_t lcd_panel_io_handle);

/// @brief Get the display's default rotation
lv_display_rotation_t lvgl_get_default_rotation();

/// @brief Interpret the given display rotation relative to the display's default rotation
lv_display_rotation_t lvgl_rotation_relative_to_default(lv_display_rotation_t rotation);
