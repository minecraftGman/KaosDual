# This file is from the Pico SDK.
# Copy it from: $PICO_SDK_PATH/external/pico_sdk_import.cmake
# Or set PICO_SDK_PATH environment variable and this file will be found automatically.

if (DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    message("Using PICO_SDK_PATH from environment: '${PICO_SDK_PATH}'")
endif ()

if (NOT PICO_SDK_PATH)
    message(FATAL_ERROR "PICO_SDK_PATH is not set. "
        "Set it via environment variable PICO_SDK_PATH or copy pico_sdk_import.cmake "
        "from the Pico SDK into your project.")
endif ()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Pico SDK")

set(PICO_SDK_INIT_CMAKE_FILE ${PICO_SDK_PATH}/pico_sdk_init.cmake)
if (NOT EXISTS ${PICO_SDK_INIT_CMAKE_FILE})
    message(FATAL_ERROR "Directory '${PICO_SDK_PATH}' does not appear to contain the Pico SDK")
endif ()

include(${PICO_SDK_INIT_CMAKE_FILE})
