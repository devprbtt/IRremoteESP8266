cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lg_hvac_modbus_telnet)

# Package web UI files from data/ into SPIFFS image.
spiffs_create_partition_image(spiffs data FLASH_IN_PROJECT)