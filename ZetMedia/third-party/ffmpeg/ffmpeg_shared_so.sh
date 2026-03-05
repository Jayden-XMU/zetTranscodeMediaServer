#!/bin/bash

# 全局配置
FFMPEG_ROOT=$(pwd)
FFMPEG_SRC_DIR="${FFMPEG_ROOT}/FFmpeg"
FFMPEG_TOOLS_DIR="${FFMPEG_ROOT}/tools"
X264_DIR="${FFMPEG_ROOT}/x264"
PREFIX="${FFMPEG_ROOT}/FFmpeg/Build/out"

# 1. 清理旧版本x264（选择性执行）
remove_old_x264() {
    if [ "$1" == "force" ]; then
        if command -v apt &> /dev/null; then
            sudo apt remove -y libx264-dev x264
        elif command -v yum &> /dev/null; then
            sudo yum remove -y x264-devel x264
        fi
        rm -rf "${X264_DIR}"
    fi
}

# 2. 智能安装x264
install_x264() {
    local NEED_REBUILD=0
    
    # 检查是否已安装
    if pkg-config --exists x264; then
        echo "检测到已安装的x264版本：$(pkg-config --modversion x264)"
        
        # 检查源码是否存在
        if [ -d "${X264_DIR}/.git" ]; then
            cd "${X264_DIR}" || return 1
            git fetch
            if [ $(git rev-parse HEAD) != $(git rev-parse @{u}) ]; then
                echo "发现x264有新版本，需要重新编译"
                NEED_REBUILD=0 # no need to rebuild if x264 has new verioin
            fi
        else
            NEED_REBUILD=1
        fi
    else
        NEED_REBUILD=1
    fi

    # 需要重新编译时执行
    if [ "$NEED_REBUILD" -eq 1 ]; then
        echo "开始编译安装x264..."
        git clone --depth 1 --branch stable https://code.videolan.org/videolan/x264.git "${X264_DIR}" || {
            [ -d "${X264_DIR}" ] && cd "${X264_DIR}" && git pull
        }
        
        cd "${X264_DIR}" || exit 1
        ./configure \
            --prefix=/usr/local \
            --enable-shared \
            --enable-pic
        
        make -j$(nproc)
        sudo make install
        sudo ldconfig
    fi
}

# 3. 依赖检查与安装
install_dependencies() {
    local DEPS=("gcc" "g++" "make" "pkg-config" "yasm" "nasm")
    local MISSING_DEPS=()
    
    # 检查缺失的依赖
    for dep in "${DEPS[@]}"; do
        if ! command -v "$dep" &> /dev/null; then
            MISSING_DEPS+=("$dep")
        fi
    done
    
    # 安装缺失的依赖
    if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
        echo "安装缺失的依赖: ${MISSING_DEPS[*]}"
        if command -v apt &> /dev/null; then
            sudo apt update
            sudo apt install -y "${MISSING_DEPS[@]}" \
                libssl-dev zlib1g-dev libsdl2-dev
        elif command -v yum &> /dev/null; then
            sudo yum install -y "${MISSING_DEPS[@]}" \
                openssl-devel zlib-devel SDL2-devel
        fi
    else
        echo "所有必需依赖已安装"
    fi
}

# 4. FFmpeg源码管理
manage_ffmpeg_source() {
    if [ -d "${FFMPEG_SRC_DIR}/.git" ]; then
        echo "检测到已存在的FFmpeg源码，尝试更新..."
        cd "${FFMPEG_SRC_DIR}" || return 1
        # 检查远程分支是否存在
            git fetch origin release/6.0
            git checkout release/6.0 -f
            git reset --hard origin/release/6.0
            echo "FFmpeg源码已更新到release/6.0"
    else
        echo "克隆FFmpeg源码..."
        git clone --depth 1 --branch release/6.0 https://github.com/FFmpeg/FFmpeg.git "${FFMPEG_SRC_DIR}" || {
            echo "错误：克隆失败，请检查网络连接"
            exit 1
        }
    fi
}

# 5. 配置与编译FFmpeg
build_ffmpeg() {
    cd "${FFMPEG_SRC_DIR}" || exit 1
    
    # 清理构建目录
    mkdir -p "${PREFIX}"
    make distclean 2>/dev/null
    
    # 配置
    echo "配置FFmpeg编译选项..."
    ./configure \
        --target-os=linux \
        --prefix="${PREFIX}" \
        --disable-shared \
        --enable-static \
        --enable-gpl \
        --enable-libx264 \
        --enable-sdl2 \
        --enable-asm \
        --disable-doc \
        --disable-vaapi \
        --disable-vdpau \
        --disable-cuda \
        --disable-cuvid \
        --disable-nvenc \
        --enable-protocol=http,https,rtmp,tcp,udp,hls \
        --extra-libs="-lpthread -lm -ldl" \
        --enable-ffplay \
        --enable-sdl2 \
        --enable-pic \
        --extra-cflags="-I/usr/local/include" \
        --extra-ldflags="-L/usr/local/lib" \
        --pkg-config-flags="--static" || {
        echo "配置失败，查看ffbuild/config.log获取详细信息"
        exit 1
    }
    
    # 编译
    echo "开始编译FFmpeg..."
    make -j$(nproc) || {
        echo "编译失败"
        exit 1
    }
    
    # 安装
    make install
    cp -f "${PREFIX}"/bin/* "${FFMPEG_TOOLS_DIR}/"
    echo "编译成功！二进制文件已复制到: ${FFMPEG_TOOLS_DIR}"
    rm ${FFMPEG_ROOT}/../../include/ffmpeg -rf
    rm ${FFMPEG_ROOT}/../../ffmpeg-lib -rf
	mkdir -p "${FFMPEG_ROOT}/../../ffmpeg-lib"
    cp ${PREFIX}/include ${FFMPEG_ROOT}/../../include/ffmpeg/ -rf
    cp ${PREFIX}/lib/*.a ${FFMPEG_ROOT}/../../ffmpeg-lib/ -rf
}

# 主执行流程
main() {
    # 选择性清理（添加force参数时才执行）
    [ "$1" == "force" ] && remove_old_x264 force
    
    install_dependencies
    install_x264
    manage_ffmpeg_source
    build_ffmpeg
}

# 执行主函数
main "$@"