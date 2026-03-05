cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_d3net)

# Package the web UI from `data/` into the SPIFFS partition on flash.
spiffs_create_partition_image(spiffs data FLASH_IN_PROJECT)
