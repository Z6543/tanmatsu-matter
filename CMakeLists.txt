# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

if(DEVICE STREQUAL "")
	set(DEVICE "$ENV{TARGET}")
endif()

if(DEVICE STREQUAL "")
	message(FATAL_ERROR "DEVICE not set")
endif()

if(DEVICE STREQUAL "tanmatsu")
	set(SDKCONFIG_DEFAULTS "sdkconfigs/general;sdkconfigs/tanmatsu")
	set(IDF_TARGET esp32p4)
elseif(DEVICE STREQUAL "mch2022")
	set(SDKCONFIG_DEFAULTS "sdkconfigs/general;sdkconfigs/mch2022")
	set(IDF_TARGET esp32)
else()
	message(FATAL_ERROR "Unknown target device ${DEVICE}")
endif()

# esp-matter integration
if(NOT DEFINED ENV{ESP_MATTER_PATH})
	message(FATAL_ERROR "Please set ESP_MATTER_PATH to the path of esp-matter repo")
endif()

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

set(EXTRA_COMPONENT_DIRS
	"${MATTER_SDK_PATH}/config/esp32/components"
	"${ESP_MATTER_PATH}/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(matter)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-D_NO_GENERIC_THREAD_NETWORK_COMMISSIONING_DRIVER_" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wformat=0;-Wno-undef" APPEND)
