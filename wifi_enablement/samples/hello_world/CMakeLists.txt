# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(FW_IMAGE_NAME "${CMAKE_CURRENT_SOURCE_DIR}/firmware/brcmfmac43455-sdio.bin" CACHE FILEPATH "Firmware binary")
set(CLM_IMAGE_NAME "${CMAKE_CURRENT_SOURCE_DIR}/firmware/brcmfmac43455-sdio.clm_blob" CACHE FILEPATH "CLM blob")
set(NVRAM_IMAGE_NAME "${CMAKE_CURRENT_SOURCE_DIR}/firmware/brcmfmac43455-sdio.txt" CACHE FILEPATH "NVRAM config")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hello_world)

target_sources(app PRIVATE src/main.c)
