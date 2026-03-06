cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hvac_modbus_gateway_bridge)

# Package web UI files from data/ into SPIFFS image.
spiffs_create_partition_image(spiffs data FLASH_IN_PROJECT)