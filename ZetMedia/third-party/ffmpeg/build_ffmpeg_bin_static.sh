#!/bin/bash

# 定义依赖安装函数
install_dependencies() {
    echo "检查并安装编译依赖..."
    if command -v apt &> /dev/null; then
        # Ubuntu/Debian系统
        sudo apt update
        # 安装FFmpeg必需的基础库和开发文件（静态编译需要静态库）
        sudo apt install -y \
            build-essential \
            gcc g++ \
            libc6-dev \
            zlib1g-dev \
            libssl-dev \
            libx264-dev \
            libx265-dev \
            pkg-config \
            libpostproc-dev \
            libsdl2-dev \
            yasm nasm \
            libx264-static \
            libx265-static \
            zlib1g-dev \
            libssl-dev
    elif command -v yum &> /dev/null; then
        # CentOS/RHEL系统
        sudo yum install -y \
            gcc gcc-c++ \
            glibc-devel \
            zlib-devel \
            openssl-devel \
            x264-devel \
            x265-devel \
            pkgconfig \
            postproc-devel \
            SDL2-devel \
            yasm nasm \
            x264-static \
            x265-static
    else
        echo "警告：未识别的包管理器，请手动安装依赖"
        exit 1
    fi
}

FFMPEG_ROOT=$(pwd)
FFMPEG_SRC_DIR="${FFMPEG_ROOT}/FFmpeg"
FFMPEG_TOOLS_DIR="${FFMPEG_ROOT}/tools"

# 克隆或更新FFmpeg源码
FFMPEG_GIT="https://github.com/FFmpeg/FFmpeg"
if [ -d "${FFMPEG_SRC_DIR}/.git" ]; then
    echo "ffmpeg source code already exists!!!"
    cd "${FFMPEG_SRC_DIR}" || exit 1
    git remote -v
    git branch
else
    git clone ${FFMPEG_GIT} 
    echo "ffmpeg clone completed"
    cd "${FFMPEG_SRC_DIR}" || exit 1
    git remote -v
    git fetch origin
    git checkout origin/release/7.0 -b release/7.0
fi

# 配置环境变量
PREFIX=${FFMPEG_ROOT}/FFmpeg/Build/out
TOOLCHAINS=/usr/bin
OS_ARCH=x86_64
OS_CROSS=x86_64-linux-gnu-  # 正确的编译器前缀
SYS_ROOT=""

# 清理并创建目录
cd ${FFMPEG_SRC_DIR}
make distclean
rm -rf ${PREFIX}
mkdir -p ${PREFIX}/bin ${FFMPEG_TOOLS_DIR}
chmod -R 777 ${PREFIX} ${FFMPEG_TOOLS_DIR}

# 配置Git安全目录
if [ -d "${FFMPEG_SRC_DIR}/.git" ]; then
    git config --global --add safe.directory "${FFMPEG_SRC_DIR}"
    echo "已配置Git安全目录: ${FFMPEG_SRC_DIR}"
fi

# 检查汇编器
check_assembler() {
    if command -v nasm &> /dev/null; then
        echo "nasm found"
        return 0
    elif command -v yasm &> /dev/null; then
        echo "yasm found"
        return 0
    else
        echo "Error: nasm/yasm not found. Installing yasm..."
        if command -v apt &> /dev/null; then
            sudo apt update && sudo apt install -y nasm
        elif command -v yum &> /dev/null; then
            sudo yum install -y nasm
        else
            echo "Error: Please install nasm or yasm manually."
            exit 1
        fi
    fi
}
check_assembler

# 安装依赖
install_dependencies

# 配置FFmpeg为静态编译
echo "开始配置静态编译FFmpeg..."
cd "${FFMPEG_SRC_DIR}" || exit 1

./configure \
    --target-os=linux \
    --prefix="${PREFIX}" \
    --enable-static \
    --disable-shared \
    --extra-cflags="-static" \
    --extra-ldflags="-static" \
    --arch="${OS_ARCH}" \
    --cc="${TOOLCHAINS}/${OS_CROSS}gcc" \
    --cxx="${TOOLCHAINS}/${OS_CROSS}g++" \
    --nm="${TOOLCHAINS}/${OS_CROSS}nm" \
    --extra-cflags="-I${PREFIX}/include \
                    -I${SYS_ROOT}/usr/include \
                    -I${SYS_ROOT}/usr/include/${OS_ARCH}-linux-gnu \
                    -O3 -fpic -std=c99 \
                    -Wno-attributes -Wno-unused-function \
                    -fno-strict-aliasing" \
    --extra-ldflags="-L${PREFIX}/lib \
                    -L${SYS_ROOT}/usr/lib \
                    -L${SYS_ROOT}/usr/lib/${OS_ARCH}-linux-gnu \
                    -lc -lm -ldl -lz -lpthread -lrt" \
    --enable-asm \
    --enable-neon \
    --disable-doc \
    --enable-pic \
    --enable-gpl \
    --enable-nonfree \
    --enable-network \
    --enable-protocol=http,https,rtmp,tcp,udp,hls \
    --enable-pthreads \
    --enable-libx264 \
    --enable-libx265 \
    --pkg-config-flags="--static" \
    --extra-libs="-lpthread -lm -ldl" \
    --enable-ffplay \
    --enable-sdl2 \
    --enable-version3 \
    --pkg-config=$(which pkg-config)

# 检查配置结果
if [ $? -ne 0 ]; then
    echo "Error: configure failed. 详细日志：${FFMPEG_SRC_DIR}/ffbuild/config.log"
    exit 1
fi

# 编译并安装
echo "配置成功，开始编译..."
make -j$(nproc)
make install

# 检查生成的二进制文件是否为静态链接
echo "检查二进制文件链接类型..."
file ${PREFIX}/bin/ffmpeg | grep "statically linked"
if [ $? -ne 0 ]; then
    echo "警告：生成的ffmpeg不是完全静态链接"
else
    echo "成功生成静态链接的ffmpeg"
fi

# 复制到工具目录
echo "复制二进制文件到工具目录..."
cp -f ${PREFIX}/bin/ffmpeg ${FFMPEG_TOOLS_DIR}/
cp -f ${PREFIX}/bin/ffprobe ${FFMPEG_TOOLS_DIR}/
cp -f ${PREFIX}/bin/ffplay ${FFMPEG_TOOLS_DIR}/
# rm ${FFMPEG_ROOT}/../../include/ffmpeg -rf
# rm ${FFMPEG_ROOT}/../../ffmpeg-lib -rf
# cp ${PREFIX}/include ${FFMPEG_ROOT}/../../include/ffmpeg/ -rf
# cp ${PREFIX}/lib/*.a ${FFMPEG_ROOT}/../../ffmpeg-lib/ -rf
echo "FFmpeg静态编译完成！二进制文件已复制到: ${FFMPEG_TOOLS_DIR}"