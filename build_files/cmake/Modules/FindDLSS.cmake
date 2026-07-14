# Locate the DLSS SDK

find_path(DLSS_INCLUDE_DIR
    NAMES
        "nvsdk_ngx.h"
    PATHS
        "${DLSS_SDK_ROOT}/include"
        "$ENV{DLSS_SDK_ROOT}/include"
)

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(DLSS REQUIRED_VARS DLSS_INCLUDE_DIR)
