set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR le32)
set(NACL TRUE)

if(NOT DEFINED NACL_SDK_ROOT)
    if(DEFINED ENV{NACL_SDK_ROOT})
        set(NACL_SDK_ROOT "$ENV{NACL_SDK_ROOT}")
    else()
        set(NACL_SDK_ROOT "/nacl_sdk/pepper_63")
    endif()
endif()

set(PNACL_BIN_DIR "${NACL_SDK_ROOT}/toolchain/linux_pnacl/bin")

set(CMAKE_C_COMPILER   "${PNACL_BIN_DIR}/pnacl-clang")
set(CMAKE_CXX_COMPILER "${PNACL_BIN_DIR}/pnacl-clang++")
set(CMAKE_AR           "${PNACL_BIN_DIR}/pnacl-ar" CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB       "${PNACL_BIN_DIR}/pnacl-ranlib" CACHE FILEPATH "Ranlib")
set(PNACL_FINALIZE     "${PNACL_BIN_DIR}/pnacl-finalize" CACHE FILEPATH "PNaCl finalize")
set(PNACL_TRANSLATE    "${PNACL_BIN_DIR}/pnacl-translate" CACHE FILEPATH "PNaCl translate")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(NACL_INCLUDE_DIRS
    "${NACL_SDK_ROOT}/include"
    "${NACL_SDK_ROOT}/include/pnacl"
)
set(NACL_LIB_DIR "${NACL_SDK_ROOT}/lib/pnacl/Release")

include_directories(SYSTEM ${NACL_INCLUDE_DIRS})
link_directories("${NACL_LIB_DIR}")

set(CMAKE_C_FLAGS_INIT "-O3 -std=gnu11 -Wno-unknown-warning-option -D_GNU_SOURCE=1")
set(CMAKE_CXX_FLAGS_INIT "-O3 -std=gnu++14 -Wno-unknown-warning-option -D_GNU_SOURCE=1")

set(CMAKE_FIND_ROOT_PATH "${NACL_SDK_ROOT}/toolchain/linux_pnacl/le32-nacl" "${NACL_SDK_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
