cmake_minimum_required(VERSION 3.10)

# ============================================================================
# Cross-compilation options: Build ARM/aarch64 products on an x86_64 host
# ============================================================================
option(ENABLE_ARM_CROSS_COMPILE "Cross compile ARM64 (dynamic library) executable files on x86_64 host" OFF)
set(HOST_ARCH_OVERRIDE "" CACHE STRING "Manually overwrite host architecture detection (Example: x86_64)")

set(_HOST_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
if(NOT _HOST_ARCH)
    set(_HOST_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(HOST_ARCH_OVERRIDE)
    set(_HOST_ARCH "${HOST_ARCH_OVERRIDE}")
else()
    if(NOT _HOST_ARCH MATCHES "x86_64|AMD64|amd64")
        execute_process(COMMAND uname -m OUTPUT_VARIABLE _UNAME_HOST_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_UNAME_HOST_ARCH)
            set(_HOST_ARCH "${_UNAME_HOST_ARCH}")
        endif()
    endif()
endif()

set(IS_CROSS_COMPILING_ARM FALSE)

if(ENABLE_ARM_CROSS_COMPILE)
    if(NOT _HOST_ARCH MATCHES "x86_64|AMD64|amd64")
        message(FATAL_ERROR "ENABLE_ARM_CROSS_COMPILE Only supported on x86_64 hosts, current host architecture: ${_HOST_ARCH}")
    endif()
    set(IS_CROSS_COMPILING_ARM TRUE)

    # Force the setting of target architecture information to ensure that subsequent logic follows the ARM branch
    set(CMAKE_SYSTEM_NAME "Linux" CACHE STRING "Cross compile target system" FORCE)
    set(CMAKE_SYSTEM_PROCESSOR "aarch64" CACHE STRING "Cross compile target architecture" FORCE)

    message(STATUS "*** Enable ARM cross compilation mode (x86_64 -> aarch64) ***")
    message(STATUS "Use toolchain path: /usr/bin/aarch64-linux-gnu-gcc/g++")

    if(NOT CMAKE_C_COMPILER OR CMAKE_C_COMPILER STREQUAL "")
        set(CMAKE_C_COMPILER "/usr/bin/aarch64-linux-gnu-gcc" CACHE FILEPATH "ARM64 Cross Compiled C Compiler" FORCE)
    endif()

    if(NOT CMAKE_CXX_COMPILER OR CMAKE_CXX_COMPILER STREQUAL "")
        set(CMAKE_CXX_COMPILER "/usr/bin/aarch64-linux-gnu-g++" CACHE FILEPATH "ARM64 Cross Compiled C++Compiler" FORCE)
    endif()

    set(CMAKE_LINKER "/usr/bin/aarch64-linux-gnu-ld" CACHE FILEPATH "ARM64 linker" FORCE)
    set(CMAKE_AR "/usr/bin/aarch64-linux-gnu-ar" CACHE FILEPATH "ARM64 Archiver" FORCE)
    set(CMAKE_NM "/usr/bin/aarch64-linux-gnu-nm" CACHE FILEPATH "ARM64 Symbol Tool" FORCE)
    set(CMAKE_OBJCOPY "/usr/bin/aarch64-linux-gnu-objcopy" CACHE FILEPATH "ARM64 Target Copy Tool" FORCE)

    # Avoid attempting to run the target program during the try_compile phase in the CMake
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
    set(CMAKE_CXX_COMPILER_WORKS TRUE)  # Skip compiler validity check
    set(CMAKE_C_COMPILER_WORKS TRUE)
elseif(CMAKE_CROSSCOMPILING)
    # Compatible with toolchain files and other cross compilation methods
    if(_HOST_ARCH MATCHES "x86_64|AMD64|amd64" AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM|AARCH64")
        set(IS_CROSS_COMPILING_ARM TRUE)
        message(STATUS "*** Detected ARM cross compilation environment with external configuration ***")
    endif()
endif()

project(zetMediaServer)
add_compile_options(-O3)

# add_compile_options(
    # -O3                    # 最高级优化，开启循环展开、指令重排等
    # -ffast-math            # 放宽数学精度，提升多媒体运算速度（FFmpeg推荐）
    # -funroll-loops         # 循环展开，减少分支跳转（适合编解码循环）
    # -finline-functions     # 函数内联，减少函数调用开销
    # -fomit-frame-pointer   # 省略帧指针，节省寄存器（提升性能）
    # -pipe                  # 使用管道减少临时文件，加速编译
# )

# # 链接阶段优化
# add_link_options(
    # -O3                    # 链接时也启用优化
    # -Wl,--as-needed        # 只链接必要的库，减小可执行文件体积
    # -Wl,--strip-all        # 剥离符号表（可选，发布时用）
# )

# # 2. 架构专属SIMD/NEON优化（区分ARMv7/ARMv8/aarch64/x86_64）
# if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    # # ARMv8/aarch64：NEON是标配，显式启用SIMD+FP16优化
    # add_compile_options(
        # -march=armv8-a+simd+fp16  # 启用ARMv8-A SIMD+半精度浮点（FFmpeg硬解码需要）
        # -mtune=cortex-a53         # 针对主流ARM核心调优（可根据目标修改为cortex-a72等）
    # )
    # add_link_options(
        # -march=armv8-a+simd+fp16
        # -mtune=cortex-a53
    # )
    # # 定义FFmpeg ARM NEON优化宏
    # add_definitions(
        # -D__ARM_NEON__
        # -D__ARM_NEON=1
        # -DFFMPEG_ARM_OPT=1
        # -DENABLE_NEON=1
    # )
    # message(STATUS "Enabled ARMv8-a/aarch64 NEON+SIMD+FP16 optimization (tune: cortex-a53)")

# elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv7")
    # # ARMv7：显式启用NEON+硬浮点ABI
    # add_compile_options(
        # -march=armv7-a
        # -mtune=cortex-a9
        # -mfpu=neon-vfpv4  # 启用NEON+VFPv4浮点单元
        # -mfloat-abi=hard  # 硬浮点ABI（性能远优于softfp）
    # )
    # add_link_options(
        # -march=armv7-a
        # -mtune=cortex-a9
        # -mfpu=neon-vfpv4
        # -mfloat-abi=hard
    # )
    # add_definitions(
        # -D__ARM_NEON__
        # -D__ARM_NEON=1
        # -DFFMPEG_ARM_OPT=1
        # -DENABLE_NEON=1
    # )
    # message(STATUS "Enabled ARMv7-a NEON+VFPv4 optimization (tune: cortex-a9)")

# elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    # # x86_64：启用全量SIMD指令集（SSE4.2/AVX2/FMA），避免-march=native（交叉编译兼容）
    # add_compile_options(
        # -msse4.2           # 基础多媒体指令集
        # -mavx2             # 高级SIMD（FFmpeg硬解码/缩放核心）
        # -mfma              # 融合乘加（提升浮点运算效率）
        # -mpopcnt           # 位计数指令
        # -mtune=generic     # 通用x86_64核心调优（兼容所有CPU）
    # )
    # add_link_options(
        # -msse4.2
        # -mavx2
        # -mfma
        # -mpopcnt
        # -mtune=generic
    # )
    # # 定义FFmpeg x86 SIMD优化宏
    # add_definitions(
        # -D__SSE4_2__
        # -D__AVX2__
        # -D__FMA__
        # -DFFMPEG_X86_OPT=1
        # -DENABLE_SIMD=1
    # )
    # message(STATUS "Enabled x86_64 SSE4.2+AVX2+FMA optimization (tune: generic)")

# else()
    # message(WARNING "Unknown architecture: ${CMAKE_SYSTEM_PROCESSOR}, skip SIMD/NEON tuning")
# endif()

# # 3. FFmpeg专属编码优化（全局宏定义）
# add_definitions(
    # -DFFMPEG_OPTIMIZE=1            # 标识启用FFmpeg优化
    # -D_HAVE_SIMD_EXTERNAL=1        # FFmpeg SIMD外部调用优化
    # -DENABLE_HWACCEL=1             # 启用FFmpeg硬件加速（硬解/硬编）
    # -DENABLE_SWSCALE_OPTIMIZATION  # 启用swscale缩放优化
    # -DENABLE_AVCODEC_OPTIMIZATION  # 启用avcodec编解码优化
# )

# # 针对架构的 SIMD 优化
# if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    # # aarch64 默认有 NEON，无需 -mfpu/-mfloat-abi
    # # 如需显式，可用 -march=armv8-a+simd
    # add_compile_options(-march=armv8-a+simd)
    # add_link_options(-march=armv8-a+simd)
# elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "armv7")
    # # 仅在 32-bit ARMv7 时使用 NEON/硬浮点
    # add_compile_options(-mfpu=neon -mfloat-abi=hard -march=armv7-a)
    # add_link_options(-mfpu=neon -mfloat-abi=hard -march=armv7-a)
# elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    # # 避免在交叉编译时使用 -march=native；可选地启用常见 SIMD
    # add_compile_options(-msse4.2 -mpopcnt)
    # add_link_options(-msse4.2 -mpopcnt)
    # # 如确认目标支持，可再加（可选）：
    # # add_compile_options(-mavx2 -mfma)
    # # add_link_options(-mavx2 -mfma)
# else()
    # message(WARNING "Unknown architecture: ${CMAKE_SYSTEM_PROCESSOR}, skip SIMD tuning")
# endif()

# ============================================================================
#Cross compilation detection: Combining toolchain and external configuration to set flag bits uniformly (after project (zetMediaServer)), 
# CMake will re evaluate variables and fill them in CMAKE_HOST_SYSTEM_PROCESSOR / CMAKE_SYSTEM_PROCESSOR，
# CMAKE_CROSSCOMPiliNG will also be updated to the correct value.The following logic is responsible for confirming
# IS_CROSS-CompILING-ARM uniformly after the "real results" are available, and outputting logs to ensure the conditions for subsequent use
#(such as FFmpeg header files and library directory selection) can correctly distinguish between cross compilation and local build
# ============================================================================
if(NOT IS_CROSS_COMPILING_ARM)
    if(CMAKE_CROSSCOMPILING)
        message(STATUS "Detected cross compilation mode")
        message(STATUS "Host architecture: ${CMAKE_HOST_SYSTEM_PROCESSOR}")
        message(STATUS "Target Architecture: ${CMAKE_SYSTEM_PROCESSOR}")
        
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND 
           CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM|AARCH64")
            set(IS_CROSS_COMPILING_ARM TRUE)
            message(STATUS "*** Cross compilation mode: Compiling ARM architecture targets on x86_64 hosts ***")
        endif()
    else()
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND 
           CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM|AARCH64")
            set(IS_CROSS_COMPILING_ARM TRUE)
            message(STATUS "*** Detected manual cross compilation configuration: Compiling ARM architecture targets on x86_64 host ***")
        endif()
    endif()
endif()

if(IS_CROSS_COMPILING_ARM)
    add_definitions(-DCROSS_COMPILE_ARM=1)
endif()

# ============================================================================
# Architecture detection: Set FFMPEG_SARCH according to the target architecture
# ============================================================================
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
   message(STATUS "x86_64 arch")
   add_definitions(-DX86_64=1)
   set(FFMPEG_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
   message(STATUS "ARM arch")
   add_definitions(-DARM=1)
   set(FFMPEG_ARCH "arm64")
else()
   message(WARNING "Unknown architecture: ${CMAKE_SYSTEM_PROCESSOR}")
   set(FFMPEG_ARCH "unknown")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
   message(STATUS "64bit system")
else()
   message(STATUS "32bit system")
endif()

set(USE_STATIC_LIB OFF)

# # Set RPATH settings globally for Linux
# if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # set(CMAKE_SKIP_BUILD_RPATH FALSE)
    # set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
    # set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    # # Enable RPATH for build tree
    # set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
    # # Ensure RPATH is used instead of RUNPATH
    # set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--disable-new-dtags")
    # set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--disable-new-dtags")

    # # Specify library path and runtime RPATH configuration when compiling links
    # link_directories("/lib/jellyfin-ffmpeg/lib")

    # # 2. Run time RPATH configuration: Ensure that the path can be found during the build phase (build directory) and after installation
    # set(CMAKE_BUILD_RPATH "/lib/jellyfin-ffmpeg/lib")    # RPATH during the build phase (when not installed)
    # set(CMAKE_INSTALL_RPATH "/lib/jellyfin-ffmpeg/lib")  # The RPATH of the program after installation
# endif()

if(USE_STATIC_LIB)
    add_definitions(-DUSE_STATIC_LIB=1)
else()
    add_definitions(-DUSE_STATIC_LIB=0)
endif()

set(ZETMEDIA_SOURCES
    ZetMedia/zetBusFactory.cpp
    ZetMedia/commonDefine.cpp
    ZetMedia/zetBusModule/zetCtlMdl.cpp
    ZetMedia/zetBusModule/zetHlsServerMdl.cpp
    ZetMedia/zetBusModule/zetFeatureServerMdl.cpp
    ZetMedia/zetBusModule/zetTranscodeMdl.cpp
    ZetMedia/zetBusModule/zetFFmpegProcess.cpp
)

if(USE_STATIC_LIB AND FFMPEG_ARCH STREQUAL "x86_64")
   message(STATUS "x86_64 static ffmpeg 6.0 lib used")
	set(ZETMEDIA_INCLUDES
		${CMAKE_SOURCE_DIR}/ZetMedia
		${CMAKE_SOURCE_DIR}/ZetMedia/zetBusModule
		${CMAKE_SOURCE_DIR}/ZetMedia/include
		${CMAKE_SOURCE_DIR}/ZetMedia/include/ffmpeg/x86_64_static_6.0
	)
elseif(NOT USE_STATIC_LIB AND FFMPEG_ARCH STREQUAL "x86_64")
     message(STATUS "x86_64 shared ffmpeg lib used")
	set(ZETMEDIA_INCLUDES
		${CMAKE_SOURCE_DIR}/ZetMedia
		${CMAKE_SOURCE_DIR}/ZetMedia/zetBusModule
		${CMAKE_SOURCE_DIR}/ZetMedia/include
		${CMAKE_SOURCE_DIR}/ZetMedia/include/ffmpeg/x86_64_shared_7.1
	)
elseif(NOT USE_STATIC_LIB AND FFMPEG_ARCH STREQUAL "arm64" AND IS_CROSS_COMPILING_ARM)
     # ============================================================================
     # Cross compile ARM: Compile ARM architecture on x86_64 host using arm_sthared_7.1 header file
     # ============================================================================
     message(STATUS "*** Cross compiling ARM64: using arm_sthared_7.1 header file ***")
	set(ZETMEDIA_INCLUDES
		${CMAKE_SOURCE_DIR}/ZetMedia
		${CMAKE_SOURCE_DIR}/ZetMedia/zetBusModule
		${CMAKE_SOURCE_DIR}/ZetMedia/include
		${CMAKE_SOURCE_DIR}/ZetMedia/include/ffmpeg/arm_shared_7.1
	)
elseif(NOT USE_STATIC_LIB AND FFMPEG_ARCH STREQUAL "arm64")
     message(STATUS "ARM64 shared ffmpeg lib used")
	set(ZETMEDIA_INCLUDES
		${CMAKE_SOURCE_DIR}/ZetMedia
		${CMAKE_SOURCE_DIR}/ZetMedia/zetBusModule
		${CMAKE_SOURCE_DIR}/ZetMedia/include
		${CMAKE_SOURCE_DIR}/ZetMedia/include/ffmpeg/arm_shared_7.1
	)
endif()

#add_library(ZetMedia SHARED ${ZETMEDIA_SOURCES})
#if(USE_STATIC_LIB)
  add_library(ZetMedia STATIC ${ZETMEDIA_SOURCES})
#else()
 # add_library(ZetMedia SHARED ${ZETMEDIA_SOURCES})
#endif()

target_include_directories(ZetMedia PRIVATE ${ZETMEDIA_INCLUDES})

# Define FFmpeg libraries
if(USE_STATIC_LIB AND FFMPEG_ARCH STREQUAL "x86_64")
    message(WARNING "now add ffmpeg staic lib")
    set(FFMPEG_LIB_DIR "${CMAKE_SOURCE_DIR}/ZetMedia/ffmpeg-lib/x86_64-static")

    if(NOT EXISTS ${FFMPEG_LIB_DIR})
        message(FATAL_ERROR "FFmpeg Library directory does not exist: ${FFMPEG_LIB_DIR}")
    endif()

	add_library(avcodec STATIC IMPORTED)
	set_target_properties(avcodec PROPERTIES
		IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavcodec.a"
	)

	add_library(avdevice STATIC IMPORTED)
	set_target_properties(avdevice PROPERTIES
		IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavdevice.a"
	)

	add_library(avfilter STATIC IMPORTED)
	set_target_properties(avfilter PROPERTIES
		IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavfilter.a"
	)

	add_library(avformat STATIC IMPORTED)
	set_target_properties(avformat PROPERTIES
	IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavformat.a"
	)

	add_library(avutil STATIC IMPORTED)
	set_target_properties(avutil PROPERTIES
	IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavutil.a"
	)
	
	add_library(swresample STATIC IMPORTED)
	set_target_properties(swresample PROPERTIES
	IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libswresample.a"
	)
	
	add_library(swscale STATIC IMPORTED)
	set_target_properties(swscale PROPERTIES
	IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libswscale.a"
	)
	
	# Link FFmpeg libraries to ZetMedia for static linking
	target_link_libraries(ZetMedia PRIVATE avformat avcodec avfilter avdevice swscale swresample avutil)
	
elseif(NOT USE_STATIC_LIB AND FFMPEG_ARCH STREQUAL "x86_64")
      message(WARNING "now add ffmpeg shared lib")
      set(FFMPEG_LIB_DIR "${CMAKE_SOURCE_DIR}/ZetMedia/ffmpeg-lib/x86_64-shared")

      if(NOT EXISTS ${FFMPEG_LIB_DIR})
        message(FATAL_ERROR "FFmpeg The dynamic library directory does not exist: ${FFMPEG_LIB_DIR}")
    endif()
    
    # Add library directory for linker to find libraries
    link_directories(${FFMPEG_LIB_DIR})
    
    add_library(avcodec SHARED IMPORTED)
    set_target_properties(avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavcodec.so"
    )
    
    add_library(avdevice SHARED IMPORTED)
    set_target_properties(avdevice PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavdevice.so"
    )
    
    add_library(avfilter SHARED IMPORTED)
    set_target_properties(avfilter PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavfilter.so"
    )
    
    add_library(avformat SHARED IMPORTED)
    set_target_properties(avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavformat.so"
    )
    
    add_library(avutil SHARED IMPORTED)
    set_target_properties(avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavutil.so"
    )
    
    add_library(swresample SHARED IMPORTED)
    set_target_properties(swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libswresample.so"
    )
    
    add_library(swscale SHARED IMPORTED)
    set_target_properties(swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libswscale.so"
    )
    
    add_library(postproc SHARED IMPORTED)
    set_target_properties(postproc PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libpostproc.so"
    )
    
    # Link FFmpeg libraries to ZetMedia for shared linking
    target_link_libraries(ZetMedia PRIVATE avformat avcodec avfilter avdevice swscale swresample avutil postproc)
    
    # Set RPATH for libZetMedia.so using linker flags
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # set_target_properties(ZetMedia PROPERTIES
            # BUILD_RPATH "${FFMPEG_LIB_DIR}"
            # INSTALL_RPATH "${FFMPEG_LIB_DIR}:$ORIGIN/../ffmpeg-lib/x86_64-shared"
            # LINK_FLAGS "-Wl,-rpath,${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN/../ffmpeg-lib/x86_64-shared"
        # )
		set_target_properties(ZetMedia PROPERTIES
			BUILD_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}"  # 合并硬件库路径
			INSTALL_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}:$ORIGIN/../ffmpeg-lib/x86_64-shared"
			LINK_FLAGS "-Wl,-rpath,/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN/../ffmpeg-lib/x86_64-shared"
		)
    endif()
elseif(NOT USE_STATIC_LIB AND FFMPEG_ARCH STREQUAL "arm64")
      message(WARNING "now add ARM64 ffmpeg shared lib")
      set(FFMPEG_LIB_DIR "${CMAKE_SOURCE_DIR}/ZetMedia/ffmpeg-lib/arm-shared")

      if(NOT EXISTS ${FFMPEG_LIB_DIR})
        message(FATAL_ERROR "ARM FFmpeg The dynamic library directory does not exist: ${FFMPEG_LIB_DIR}")
    endif()
    
    # Add library directory for linker to find libraries
    link_directories(${FFMPEG_LIB_DIR})
    
    add_library(avcodec SHARED IMPORTED)
    set_target_properties(avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavcodec.so"
    )
    
    add_library(avdevice SHARED IMPORTED)
    set_target_properties(avdevice PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavdevice.so"
    )
    
    add_library(avfilter SHARED IMPORTED)
    set_target_properties(avfilter PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavfilter.so"
    )
    
    add_library(avformat SHARED IMPORTED)
    set_target_properties(avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavformat.so"
    )
    
    add_library(avutil SHARED IMPORTED)
    set_target_properties(avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavutil.so"
    )
    
    add_library(swresample SHARED IMPORTED)
    set_target_properties(swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libswresample.so"
    )
    
    add_library(swscale SHARED IMPORTED)
    set_target_properties(swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libswscale.so"
    )
    
    add_library(postproc SHARED IMPORTED)
    set_target_properties(postproc PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libpostproc.so"
    )
    
    # Link FFmpeg libraries to ZetMedia for shared linking
    target_link_libraries(ZetMedia PRIVATE avformat avcodec avfilter avdevice swscale swresample avutil postproc)
    
    # Set RPATH for libZetMedia.so using linker flags
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # set_target_properties(ZetMedia PROPERTIES
            # BUILD_RPATH "${FFMPEG_LIB_DIR}"
            # INSTALL_RPATH "${FFMPEG_LIB_DIR}:$ORIGIN/../ffmpeg-lib/arm-shared"
            # LINK_FLAGS "-Wl,-rpath,${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN/../ffmpeg-lib/arm-shared"
        # )
        set_target_properties(ZetMedia PROPERTIES
            BUILD_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}"  # 合并硬件库路径
            INSTALL_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}:$ORIGIN/../ffmpeg-lib/arm-shared"
            LINK_FLAGS "-Wl,-rpath,/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN/../ffmpeg-lib/arm-shared"
        )
    endif()
elseif(NOT USE_STATIC_LIB AND FFMPEG_ARCH STREQUAL "arm64" AND IS_CROSS_COMPILING_ARM)
    # ============================================================================
    # Cross compile ARM: Compile ARM architecture on x86_64 host using dynamic libraries in arm shared directory
    # ============================================================================
    message(WARNING "*** Cross compile ARM64: Add FFmpeg shared library (arm shared directory)***")
    set(FFMPEG_LIB_DIR "${CMAKE_SOURCE_DIR}/ZetMedia/ffmpeg-lib/arm-shared")

    if(NOT EXISTS ${FFMPEG_LIB_DIR})
        message(FATAL_ERROR "Cross compile ARM: FFmpeg dynamic library directory does not exist: ${FFMPEG_LIB_DIR}")
    endif()
    
    # Add library directory for linker to find libraries
    link_directories(${FFMPEG_LIB_DIR})
    
    add_library(avcodec SHARED IMPORTED)
    set_target_properties(avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavcodec.so"
    )
    
    add_library(avdevice SHARED IMPORTED)
    set_target_properties(avdevice PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavdevice.so"
    )
    
    add_library(avfilter SHARED IMPORTED)
    set_target_properties(avfilter PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavfilter.so"
    )
    
    add_library(avformat SHARED IMPORTED)
    set_target_properties(avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavformat.so"
    )
    
    add_library(avutil SHARED IMPORTED)
    set_target_properties(avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libavutil.so"
    )
    
    add_library(swresample SHARED IMPORTED)
    set_target_properties(swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libswresample.so"
    )
    
    add_library(swscale SHARED IMPORTED)
    set_target_properties(swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libswscale.so"
    )
    
    add_library(postproc SHARED IMPORTED)
    set_target_properties(postproc PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/libpostproc.so"
    )
    
    # Link FFmpeg libraries to ZetMedia for shared linking (cross-compile ARM)
    target_link_libraries(ZetMedia PRIVATE avformat avcodec avfilter avdevice swscale swresample avutil postproc)
    
    # Set RPATH for libZetMedia.so using linker flags (cross-compile ARM)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # set_target_properties(ZetMedia PROPERTIES
            # BUILD_RPATH "${FFMPEG_LIB_DIR}"
            # INSTALL_RPATH "${FFMPEG_LIB_DIR}:$ORIGIN/../ffmpeg-lib/arm-shared"
            # LINK_FLAGS "-Wl,-rpath,${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN/../ffmpeg-lib/arm-shared"
        # )
        set_target_properties(ZetMedia PROPERTIES
            BUILD_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}"
            INSTALL_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}:$ORIGIN/../ffmpeg-lib/arm-shared"
            LINK_FLAGS "-Wl,-rpath,/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN/../ffmpeg-lib/arm-shared"
        )
    endif()
endif()

add_executable(zetMediaServer zetMediaServer.cpp)

if(USE_STATIC_LIB  AND FFMPEG_ARCH STREQUAL "x86_64")
   message(STATUS "ffmpeg ready to link staic lib...")
   set(CMAKE_EXE_LINKER_FLAGS "-static")
   set(CMAKE_EXE_LINK_DYNAMIC_LIBS OFF)
   set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
   set(BUILD_SHARED_LIBS OFF)
   target_link_libraries(zetMediaServer PRIVATE ZetMedia avformat avcodec avfilter avdevice swscale swresample avutil x264 pthread dl z m)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set_target_properties(zetMediaServer PROPERTIES
            BUILD_RPATH "${FFMPEG_LIB_DIR}"
        )
    endif()
 elseif(NOT USE_STATIC_LIB AND FFMPEG_ARCH STREQUAL "arm64" AND IS_CROSS_COMPILING_ARM)
     # ============================================================================
     # Cross compiling ARM: compiling ARM architecture executable files on x86_64 hosts
     # ============================================================================
     message(STATUS "*** Cross compiling ARM64: Linking shared libraries to executable files ***")
    # For dynamic linking, ZetMedia already links to FFmpeg libraries
    target_link_libraries(zetMediaServer PRIVATE ZetMedia pthread)
    
    # Set RPATH for zetMediaServer executable using linker flags (cross-compile ARM)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # set_target_properties(zetMediaServer PROPERTIES
            # BUILD_RPATH "${FFMPEG_LIB_DIR}:$ORIGIN"
            # INSTALL_RPATH "${FFMPEG_LIB_DIR}:$ORIGIN:$ORIGIN/../ZetMedia/ffmpeg-lib/arm-shared"
            # LINK_FLAGS "-Wl,-rpath,${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN -Wl,-rpath,$ORIGIN/../ZetMedia/ffmpeg-lib/arm-shared"
        # )
		set_target_properties(zetMediaServer PROPERTIES
			BUILD_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}:$ORIGIN"
			INSTALL_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}:$ORIGIN:$ORIGIN/../ZetMedia/ffmpeg-lib/arm-shared"
			LINK_FLAGS "-Wl,-rpath,/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN -Wl,-rpath,$ORIGIN/../ZetMedia/ffmpeg-lib/arm-shared"
		)
    endif()
 elseif(NOT USE_STATIC_LIB AND FFMPEG_ARCH STREQUAL "arm64")
     message(STATUS "ARM64 shared ffmpeg lib used for linking...")
    # For dynamic linking, ZetMedia already links to FFmpeg libraries
    target_link_libraries(zetMediaServer PRIVATE ZetMedia pthread)
    
    # Set RPATH for zetMediaServer executable using linker flags
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # set_target_properties(zetMediaServer PROPERTIES
            # BUILD_RPATH "${FFMPEG_LIB_DIR}:$ORIGIN"
            # INSTALL_RPATH "${FFMPEG_LIB_DIR}:$ORIGIN:$ORIGIN/../ZetMedia/ffmpeg-lib/arm-shared"
            # LINK_FLAGS "-Wl,-rpath,${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN -Wl,-rpath,$ORIGIN/../ZetMedia/ffmpeg-lib/arm-shared"
        # )
		set_target_properties(zetMediaServer PROPERTIES
			BUILD_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}:$ORIGIN"
			INSTALL_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}:$ORIGIN:$ORIGIN/../ZetMedia/ffmpeg-lib/arm-shared"
			LINK_FLAGS "-Wl,-rpath,/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN -Wl,-rpath,$ORIGIN/../ZetMedia/ffmpeg-lib/arm-shared"
		)
    endif()
 else()
     message(STATUS "ffmpeg ready to link shared lib...")
    # For dynamic linking, ZetMedia already links to FFmpeg libraries
    target_link_libraries(zetMediaServer PRIVATE ZetMedia pthread)
    
    # Set RPATH for zetMediaServer executable using linker flags
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # set_target_properties(zetMediaServer PROPERTIES
            # BUILD_RPATH "${FFMPEG_LIB_DIR}:$ORIGIN"
            # INSTALL_RPATH "${FFMPEG_LIB_DIR}:$ORIGIN:$ORIGIN/../ZetMedia/ffmpeg-lib/x86_64-shared"
            # LINK_FLAGS "-Wl,-rpath,${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN -Wl,-rpath,$ORIGIN/../ZetMedia/ffmpeg-lib/x86_64-shared"
        # )
		set_target_properties(zetMediaServer PROPERTIES
			# 合并硬件库路径
			BUILD_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}:$ORIGIN"
			INSTALL_RPATH "/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR}:$ORIGIN:$ORIGIN/../ZetMedia/ffmpeg-lib/x86_64-shared"
			LINK_FLAGS "-Wl,-rpath,/lib/jellyfin-ffmpeg/lib:${FFMPEG_LIB_DIR} -Wl,-rpath,$ORIGIN -Wl,-rpath,$ORIGIN/../ZetMedia/ffmpeg-lib/x86_64-shared"
		)
    endif()
 endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
   target_compile_definitions(zetMediaServer PUBLIC "LINUX")
   message("running on Linux os!")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
   target_compile_definitions(zetMediaServer PUBLIC "WINDOWS")
   message("running on Windows os!")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
   target_compile_definitions(zetMediaServer PUBLIC "MACOS")
   message("running on mac os!")
else()
   message("running on unknown os!")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Intel")
   target_compile_definitions(zetMediaServer PUBLIC "IS_INTEL_CXX_COMPILER")
   message("using Intel CXX compiler !!!")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
   target_compile_definitions(zetMediaServer PUBLIC "IS_GNU_CXX_COMPILER")
   message("using GNU CXX compiler !!!")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "XL")
   target_compile_definitions(zetMediaServer PUBLIC "IS_XL_CXX_COMPILER")
   message("using XL CXX compiler !!!")
else()
   message("running on unknown compiler!")
endif()

target_include_directories(zetMediaServer PRIVATE ${ZETMEDIA_INCLUDES})

# Set log level macros (must be placed after add_ibrary and add_deecutable)
if(NOT DEFINED LOG_LEVEL)
    set(LOG_LEVEL "DEBUG")
endif()

if(LOG_LEVEL STREQUAL "DEBUG")
    set(LOG_LEVEL_VALUE "LOG_DEBUG")
elseif(LOG_LEVEL STREQUAL "INFO")
    set(LOG_LEVEL_VALUE "LOG_INFO")
elseif(LOG_LEVEL STREQUAL "WARNING")
    set(LOG_LEVEL_VALUE "LOG_WARNING")
elseif(LOG_LEVEL STREQUAL "ERROR")
    set(LOG_LEVEL_VALUE "LOG_ERROR")
elseif(LOG_LEVEL STREQUAL "CRITICAL")
    set(LOG_LEVEL_VALUE "LOG_CRITICAL")
else()
    message(WARNING "Unknown LOG_LEVEL=${LOG_LEVEL}, defaulting to INFO")
    set(LOG_LEVEL_VALUE 1)
endif()

target_compile_definitions(ZetMedia PRIVATE CURRENT_LOG_LEVEL=${LOG_LEVEL_VALUE})
target_compile_definitions(zetMediaServer PRIVATE CURRENT_LOG_LEVEL=${LOG_LEVEL_VALUE})
