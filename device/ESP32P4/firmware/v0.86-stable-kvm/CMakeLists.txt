# The following boilerplate must be in this order for ESP-IDF CMake projects.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.86.0-stable-kvm")
project(exoanchor_kvm_esp32p4)

# usb_host_uvc 2.5.1 relies on the UVC EOF flag for isochronous MJPEG.
# Some HDMI capture cards omit that flag, so use the compatibility source
# that completes frames from JPEG SOI/EOI markers instead. Keep the upstream
# hash guard: a dependency update must be reviewed before this override is
# carried forward.
idf_component_get_property(si_uvc_component espressif__usb_host_uvc COMPONENT_LIB)
idf_component_get_property(si_uvc_component_dir espressif__usb_host_uvc COMPONENT_DIR)
set(si_uvc_upstream_isoc "${si_uvc_component_dir}/uvc_isoc.c")
set(si_uvc_compat_isoc "${CMAKE_CURRENT_LIST_DIR}/patches/usb_host_uvc-2.5.1/uvc_isoc.c")
set(si_uvc_251_isoc_sha256 "c709685f2814697c568620c7c2b7974e3e3167a92308ddd324c46db082de1ccc")

file(SHA256 "${si_uvc_upstream_isoc}" si_uvc_upstream_isoc_sha256)
if(NOT si_uvc_upstream_isoc_sha256 STREQUAL si_uvc_251_isoc_sha256)
    message(FATAL_ERROR
        "usb_host_uvc/uvc_isoc.c changed; review the ExoAnchor MJPEG compatibility override")
endif()

get_target_property(si_uvc_sources ${si_uvc_component} SOURCES)
list(FIND si_uvc_sources "${si_uvc_upstream_isoc}" si_uvc_upstream_isoc_index)
if(si_uvc_upstream_isoc_index EQUAL -1)
    message(FATAL_ERROR "Could not replace usb_host_uvc isochronous source")
endif()
list(REMOVE_ITEM si_uvc_sources "${si_uvc_upstream_isoc}")
list(APPEND si_uvc_sources "${si_uvc_compat_isoc}")
set_property(TARGET ${si_uvc_component} PROPERTY SOURCES "${si_uvc_sources}")
message(STATUS "Using ExoAnchor UVC MJPEG SOI/EOI compatibility source")
