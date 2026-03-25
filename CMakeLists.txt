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

# Tanmatsu uses nicolaielectronics__esp-hosted-tanmatsu (a fork with SDIO/RPC patches).
# Replace espressif__esp_hosted sources with a dummy file so the target is a valid
# but empty static library — this prevents duplicate symbol conflicts since the
# tanmatsu fork provides all required symbols.
if(TARGET __idf_espressif__esp_hosted)
	file(WRITE "${CMAKE_BINARY_DIR}/__esp_hosted_dummy.c" "/* replaced by nicolaielectronics__esp-hosted-tanmatsu */\n")
	set_property(TARGET __idf_espressif__esp_hosted PROPERTY SOURCES "${CMAKE_BINARY_DIR}/__esp_hosted_dummy.c")
endif()

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-D_NO_GENERIC_THREAD_NETWORK_COMMISSIONING_DRIVER_" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wformat=0;-Wno-undef" APPEND)

# The prebuilt libopenthread_br.a references mDNS symbols but the
# openthread component does not declare that dependency for the
# border router case. Fix the link order here.
if(CONFIG_OPENTHREAD_BORDER_ROUTER)
	idf_component_get_property(mdns_lib espressif__mdns COMPONENT_LIB)
	idf_component_get_property(ot_lib openthread COMPONENT_LIB)
	target_link_libraries(${ot_lib} PUBLIC ${mdns_lib})
endif()
