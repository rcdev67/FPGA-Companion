cmake_minimum_required(VERSION 3.15)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Export compile commands for tooling" FORCE)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED TANG_BOARD OR "${TANG_BOARD}" STREQUAL "")
    set(TANG_BOARD "m0sdock")
endif()

if(TANG_BOARD STREQUAL "mega60k")
    add_definitions(-DTANG_MEGA60K)
elseif(TANG_BOARD STREQUAL "mega138kpro")
    add_definitions(-DTANG_MEGA138KPRO)
elseif(TANG_BOARD STREQUAL "console60k") 
    add_definitions(-DTANG_CONSOLE60K)
elseif(TANG_BOARD STREQUAL "console138k")
    add_definitions(-DTANG_CONSOLE138K)
elseif(TANG_BOARD STREQUAL "primer25k")
    add_definitions(-DTANG_PRIMER25K)
elseif(TANG_BOARD STREQUAL "nano20k")
    add_definitions(-DTANG_NANO20K)
elseif(TANG_BOARD STREQUAL "nano20k_v3923")
    add_definitions(-DTANG_NANO20K_V3923)
elseif(TANG_BOARD STREQUAL "m0sdock")
    add_definitions(-DM0S_DOCK)
else()
   message(FATAL_ERROR "TANG_BOARD must be mega60k, mega138kpro, console60k, primer25k, nano20k, nano20k_v3923 or m0sdock")
endif()

# The commit the companion is built from, for the device status it sends
# stserve (net.c, status_query()). "unknown" without git.
execute_process(COMMAND git rev-parse --short=7 HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE COMPANION_GIT
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT COMPANION_GIT)
    set(COMPANION_GIT "unknown")
endif()
add_definitions(-DCOMPANION_VERSION=\"${COMPANION_GIT}\")

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})
sdk_add_include_directories($ENV{BL_SDK_BASE}/components/net/lwip/lwip/src/include)
sdk_add_include_directories(. ../u8g2/csrc)

sdk_add_link_options(-Wl,--wrap=bflb_console_write)

# User
sdk_add_compile_definitions(-DCONFIG_CLI_CMD_ENABLE)

target_sources(app PRIVATE
   	mcu_hw.c
    $ENV{BL_SDK_BASE}/components/net/lwip/lwip/src/apps/sntp/sntp.c
    ../ftpd.c
    ../telnetd.c
    ../net.c
    ../hidparser.c
   	../hid.c
   	../inifile.c
   	../menu.c
   	../sdc.c
   	../osd_u8g2.c
   	../sysctrl.c
   	../config.c
   	../xml.c
    ../at_wifi.c
    ../puff.c
    ../jtag.c
    hid_keyboard_template.c
    ../gowin.c
    sdc_direct.c
    bt_usb.c
    bt_hid.c
    bt_keys.c
    bt_tlv.c
    bt_rtk.c
    ../ps2helper.c
)

# The Realtek RTL8761BU's firmware patch, loaded into the adapter by
# bt_rtk.c. Realtek's files from linux-firmware, unmodified, with their
# licence in firmware/rtl_bt; turned into C arrays here, at configure time,
# so the build output carries them and the sources keep the original files.
function(embed_binary file name)
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/${file} hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}.c)
  file(WRITE ${out} "/* generated from ${file} by CMakeLists.txt */\nconst unsigned char ${name}[] = {${bytes}};\nconst unsigned int ${name}_len = sizeof(${name});\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${file})
  target_sources(app PRIVATE ${out})
endfunction()
embed_binary(firmware/rtl_bt/rtl8761bu_fw.bin rtl8761bu_fw)
embed_binary(firmware/rtl_bt/rtl8761bu_config.bin rtl8761bu_config)

file(GLOB COMPONENT_SRCS ../u8g2/csrc/*.c  ../u8g2/sys/bitmap/common/*.c)
target_sources(app PRIVATE ${COMPONENT_SRCS})

# Bluetooth through a USB adapter: BTstack (../btstack, a submodule; free
# for non-commercial use, see its LICENSE) as a HID host, classic and Low
# Energy. Its configuration is btstack_config.h in this directory.
set(BTSTACK ../btstack)
sdk_add_include_directories(${BTSTACK}/src ${BTSTACK}/platform/freertos ${BTSTACK}/platform/embedded ${BTSTACK}/3rd-party/micro-ecc)
sdk_add_compile_definitions(-DENABLE_BLUETOOTH)

# The heap is short: four Bluetooth devices take some 13 KB of it. The
# menu's thread used 0.6 KB of its 16 KB, measured with the OSD open and a
# Bluetooth scan running; 8 KB it is. The shell's thread, in defconfig,
# likewise.
sdk_add_compile_definitions(-DMENU_TASK_STACK=2048)

# The FPGA communication and the net task had 16 KB each and used 3.2 KB
# and 3.1 KB, measured with the C64, three Bluetooth devices and the report
# bt_hid.c prints a minute after the start. With 8 KB each the heap has
# room for the third Bluetooth device, which it had not.
sdk_add_compile_definitions(-DCOM_TASK_STACK=2048 -DNET_TASK_STACK=2048)
target_sources(app PRIVATE
    ${BTSTACK}/src/ad_parser.c
    ${BTSTACK}/src/btstack_crypto.c
    ${BTSTACK}/src/btstack_hid.c
    ${BTSTACK}/src/btstack_hid_parser.c
    ${BTSTACK}/src/btstack_linked_list.c
    ${BTSTACK}/src/btstack_memory.c
    ${BTSTACK}/src/btstack_memory_pool.c
    ${BTSTACK}/src/btstack_run_loop.c
    ${BTSTACK}/src/btstack_run_loop_base.c
    ${BTSTACK}/src/btstack_tlv.c
    ${BTSTACK}/src/btstack_util.c
    ${BTSTACK}/src/hci.c
    ${BTSTACK}/src/hci_cmd.c
    ${BTSTACK}/src/hci_dump.c
    ${BTSTACK}/src/hci_event.c
    ${BTSTACK}/src/hci_event_builder.c
    ${BTSTACK}/src/l2cap.c
    ${BTSTACK}/src/l2cap_signaling.c
    ${BTSTACK}/src/classic/btstack_link_key_db_memory.c
    ${BTSTACK}/src/classic/hid_host.c
    ${BTSTACK}/src/classic/sdp_client.c
    ${BTSTACK}/src/classic/sdp_util.c
    ${BTSTACK}/src/ble/att_db.c
    ${BTSTACK}/src/ble/att_db_util.c
    ${BTSTACK}/src/ble/att_dispatch.c
    ${BTSTACK}/src/ble/att_server.c
    ${BTSTACK}/src/ble/gatt_client.c
    ${BTSTACK}/src/ble/le_device_db_tlv.c
    ${BTSTACK}/src/ble/sm.c
    ${BTSTACK}/src/ble/gatt-service/hids_host.c
    ${BTSTACK}/platform/freertos/btstack_run_loop_freertos.c
    ${BTSTACK}/3rd-party/micro-ecc/uECC.c
)

sdk_set_main_file(../main.c)

project(fpga_companion)
#project(fpga_companion CXX)
