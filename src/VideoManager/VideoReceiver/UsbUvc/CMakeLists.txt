# Direct USB UVC capture on Android (cameras/analog grabbers that Android's camera API does not expose).
# The device is opened from Java (USB permission) and read with libusb + libuvc on the file descriptor.
if(NOT ANDROID)
    return()
endif()

CPMAddPackage(
    NAME libusb
    GITHUB_REPOSITORY libusb/libusb
    GIT_TAG v1.0.27
    DOWNLOAD_ONLY YES
)

CPMAddPackage(
    NAME libuvc
    GITHUB_REPOSITORY libuvc/libuvc
    GIT_TAG 448d78b9aa4f54172c1ca5b5d1b4a6b9b93f7957
    DOWNLOAD_ONLY YES
)

# libusb (same file list as its android/jni/libusb.mk)
add_library(qgc_libusb STATIC
    ${libusb_SOURCE_DIR}/libusb/core.c
    ${libusb_SOURCE_DIR}/libusb/descriptor.c
    ${libusb_SOURCE_DIR}/libusb/hotplug.c
    ${libusb_SOURCE_DIR}/libusb/io.c
    ${libusb_SOURCE_DIR}/libusb/sync.c
    ${libusb_SOURCE_DIR}/libusb/strerror.c
    ${libusb_SOURCE_DIR}/libusb/os/linux_usbfs.c
    ${libusb_SOURCE_DIR}/libusb/os/events_posix.c
    ${libusb_SOURCE_DIR}/libusb/os/threads_posix.c
    ${libusb_SOURCE_DIR}/libusb/os/linux_netlink.c
)
target_include_directories(qgc_libusb
    PRIVATE
        ${libusb_SOURCE_DIR}/android
        ${libusb_SOURCE_DIR}/libusb/os
    PUBLIC
        ${libusb_SOURCE_DIR}/libusb
)
target_compile_options(qgc_libusb PRIVATE -fvisibility=hidden -pthread -w)
target_link_libraries(qgc_libusb PRIVATE log)

# libuvc (without its optional libjpeg support)
set(libuvc_VERSION_MAJOR 0)
set(libuvc_VERSION_MINOR 0)
set(libuvc_VERSION_PATCH 9)
set(libuvc_VERSION 0.0.9)
configure_file(${libuvc_SOURCE_DIR}/include/libuvc/libuvc_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/libuvc_include/libuvc/libuvc_config.h)

# Copy of stream.c with diagnostic hooks (implemented in UsbUvcCamera.cc) so USB transfer problems
# can be seen on screen: libuvc silently drops failed transfers/packets.
file(READ ${libuvc_SOURCE_DIR}/src/stream.c UVC_STREAM_SRC)
string(REPLACE
    "#include \"libuvc/libuvc_internal.h\""
    "#include \"libuvc/libuvc_internal.h\"\nvoid qgc_uvc_transfer_hook(struct libusb_transfer *transfer);\nvoid qgc_uvc_altsetting_hook(int alt, int bytesPerPacket, int packetsPerTransfer, int configBytesPerPacket);\nvoid qgc_uvc_submit_hook(int submitted, int ret);"
    UVC_STREAM_SRC "${UVC_STREAM_SRC}")
string(REPLACE
    "  int resubmit = 1;"
    "  int resubmit = 1;\n  qgc_uvc_transfer_hook(transfer);"
    UVC_STREAM_SRC "${UVC_STREAM_SRC}")
string(REPLACE
    "    /* Select the altsetting */"
    "    qgc_uvc_altsetting_hook(altsetting->bAlternateSetting, (int) endpoint_bytes_per_packet, (int) packets_per_transfer, (int) config_bytes_per_packet);\n    /* Select the altsetting */"
    UVC_STREAM_SRC "${UVC_STREAM_SRC}")
string(REPLACE
    "  if ( ret != UVC_SUCCESS && transfer_id >= 0 ) {"
    "  qgc_uvc_submit_hook(transfer_id, ret);\n  if ( ret != UVC_SUCCESS && transfer_id >= 0 ) {"
    UVC_STREAM_SRC "${UVC_STREAM_SRC}")
foreach(UVC_HOOK qgc_uvc_transfer_hook\(transfer\) qgc_uvc_altsetting_hook\(altsetting qgc_uvc_submit_hook\(transfer_id)
    string(FIND "${UVC_STREAM_SRC}" "${UVC_HOOK}" UVC_HOOK_POS)
    if(UVC_HOOK_POS EQUAL -1)
        message(FATAL_ERROR "libuvc stream.c diagnostic patch failed: ${UVC_HOOK}")
    endif()
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/libuvc_patched/stream.c "${UVC_STREAM_SRC}")

add_library(qgc_libuvc STATIC
    ${libuvc_SOURCE_DIR}/src/ctrl.c
    ${libuvc_SOURCE_DIR}/src/ctrl-gen.c
    ${libuvc_SOURCE_DIR}/src/device.c
    ${libuvc_SOURCE_DIR}/src/diag.c
    ${libuvc_SOURCE_DIR}/src/frame.c
    ${libuvc_SOURCE_DIR}/src/init.c
    ${CMAKE_CURRENT_BINARY_DIR}/libuvc_patched/stream.c
    ${libuvc_SOURCE_DIR}/src/misc.c
)
target_include_directories(qgc_libuvc
    PUBLIC
        ${libuvc_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/libuvc_include
)
target_compile_options(qgc_libuvc PRIVATE -w)
# libuvc queues 100 transfers by default (20 on macOS); Android USB host controllers time out
# with that many large isochronous transfers queued
target_compile_definitions(qgc_libuvc PRIVATE LIBUVC_NUM_TRANSFER_BUFS=20)
target_link_libraries(qgc_libuvc PUBLIC qgc_libusb)

target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        UsbUvcCamera.cc
        UsbUvcCamera.h
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE qgc_libuvc)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE QGC_USB_UVC)
