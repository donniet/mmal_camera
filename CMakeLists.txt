cmake_minimum_required(VERSION 2.8)

project(mmal_camera)

include_directories(include)

# Global compiler flags
if(CMAKE_COMPILER_IS_GNUCC)
   set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-multichar -Wall -Wno-unused-but-set-variable -fPIC")
   set(CMAKE_CXX_FLAGS "${CMAKE_CPP_FLAGS} -std=c++11 -Wno-multichar -Wall -Wno-unused-but-set-variable -fPIC")
endif()

# set(vmcs_root ${USERLAND})
# get_filename_component(VIDEOCORE_ROOT . ABSOLUTE)

# include("${USERLAND}/makefiles/cmake/global_settings.cmake")
# include("${USERLAND}/makefiles/cmake/arm-linux.cmake")
# include("${USERLAND}/makefiles/cmake/vmcs.cmake")

SET(CMAKE_SYSTEM_NAME Linux)
SET(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
SET(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
SET(CMAKE_ASM_COMPILER arm-linux-gnueabihf-gcc)
SET(CMAKE_SYSTEM_PROCESSOR arm)

#ADD_DEFINITIONS("-march=armv6")
add_definitions("-mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard -marm")

# rdynamic means the backtrace should work
IF (CMAKE_BUILD_TYPE MATCHES "Debug")
   add_definitions(-rdynamic)
ENDIF()

# avoids annoying and pointless warnings from gcc
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -U_FORTIFY_SOURCE")
SET(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -c")


add_definitions(-D_REENTRANT)
add_definitions(-DUSE_VCHIQ_ARM -DVCHI_BULK_ALIGN=1 -DVCHI_BULK_GRANULARITY=1)
add_definitions(-DOMX_SKIP64BIT)
add_definitions(-DEGL_SERVER_DISPMANX)
add_definitions(-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64)
add_definitions(-D_GNU_SOURCE)

# do we actually need this?
add_definitions(-D__VIDEOCORE4__)
add_definitions(-DTV_SUPPORTED_MODE_NO_DEPRECATED)

link_directories(/opt/vc/lib)
include_directories(/opt/vc/include)

add_executable(mmal_camera  main.cpp)

# set (MMAL_LIBS mmal_core mmal_util mmal_vc_client)

target_link_libraries(mmal_camera mmal mmal_core mmal_components mmal_vc_client mmal_util vcos bcm_host)