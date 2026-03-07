idf_component_register(
	SRCS
		"main.c"
		"device_manager.c"
		"ui_screens.c"
		"matter_init.cpp"
		"matter_commission.cpp"
		"matter_device_control.cpp"
		"bsp_lvgl.c"
	PRIV_REQUIRES
		esp_lcd
		esp_timer
		driver
		esp_psram
		esp_driver_ppa
		fatfs
		nvs_flash
		badge-bsp
		chip
		esp_matter
		esp_matter_controller
		esp_wifi
	INCLUDE_DIRS
		"."
)

idf_component_get_property(lvgl_lib lvgl COMPONENT_LIB)
idf_component_get_property(libpng_lib espressif__libpng COMPONENT_LIB)
target_link_libraries(${lvgl_lib} PUBLIC ${libpng_lib})
