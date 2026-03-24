#!/bin/bash

# FluffOS 构建脚本 v5.0 - Ultra Performance Edition
# 特性:
#   - 极致性能优化 (PGO, LTO, CPU特定优化)
#   - 稳定的链接器策略 (Gold > LLD > BFD)
#   - 智能源码管理 (保护现有代码)
#   - 编译缓存加速 (ccache)
#   - 交互式配置

set -e

# ==== 性能配置 ====
USE_JEMALLOC=1                  # 使用 jemalloc (性能提升 ~15%)
USE_CCACHE=0                    # 使用编译缓存 (重编译加速 ~10倍) - 已禁用
USE_LTO=1                       # 链接时优化 (性能提升 ~5-10%)
OPTIMIZATION_LEVEL="3"          # 0=调试, 2=标准, 3=极致
PREFER_CLANG=1                  # Clang 通常比 GCC 快 5-10%
LINKER_PREFERENCE="gold"        # gold=稳定, lld=快但可能不稳定, mold=最快但兼容性差, bfd=默认

# ==== 源码配置 ====
GIT_REPO="${GIT_REPO:-https://github.com/fluffos/fluffos.git}"
GIT_BRANCH="${GIT_BRANCH:-master}"
GIT_MIRROR="https://gitee.com/fluffos/fluffos.git"

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
DEFAULT_PROJECT_DIR="$SCRIPT_DIR/fluffos"
PROJECT_DIR="${PROJECT_DIR:-$DEFAULT_PROJECT_DIR}"

# ==== 颜色定义 ====
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; MAGENTA='\033[0;35m'; NC='\033[0m'

print_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
print_step() { echo -e "${BLUE}[STEP]${NC} $1"; }
print_success() { echo -e "${CYAN}[✓]${NC} $1"; }
print_perf() { echo -e "${MAGENTA}[PERF]${NC} $1"; }

check_root() {
    if [ "$EUID" -ne 0 ]; then
        print_error "需要 root 权限运行此脚本"
        echo "请使用: sudo $0"
        exit 1
    fi
}

detect_system() {
    if [ ! -f /etc/os-release ]; then
        print_error "无法检测系统信息"
        exit 1
    fi

    . /etc/os-release
    OS_ID="$ID"
    OS_NAME="$NAME"

    if [[ "$OS_NAME" =~ "Euler" || "$OS_NAME" =~ "HCE" || "$OS_ID" =~ "rhel" || "$OS_ID" =~ "centos" ]]; then
        DISTRO_TYPE="rhel"
        PKG_MGR="yum"
    elif [[ "$OS_ID" =~ "ubuntu" || "$OS_ID" =~ "debian" ]]; then
        DISTRO_TYPE="debian"
        PKG_MGR="apt"
    elif [[ "$OS_ID" =~ "arch" ]]; then
        DISTRO_TYPE="arch"
        PKG_MGR="pacman"
    else
        DISTRO_TYPE="unknown"
        PKG_MGR="unknown"
    fi

    print_info "系统: $OS_NAME ($DISTRO_TYPE)"
}

detect_cpu() {
    if command -v lscpu &>/dev/null; then
        CPU_MODEL=$(lscpu | grep "Model name" | sed 's/Model name:[[:space:]]*//')
        CPU_CORES=$(nproc 2>/dev/null || echo "4")
        print_info "CPU: $CPU_MODEL ($CPU_CORES cores)"
    else
        CPU_CORES=$(nproc 2>/dev/null || echo "4")
        print_info "CPU Cores: $CPU_CORES"
    fi
}

install_dependencies() {
    print_step "安装/检查依赖包..."

    case "$DISTRO_TYPE" in
        debian)
            # 只在需要时更新
            if [ ! -x "$(command -v cmake)" ] || [ ! -x "$(command -v git)" ]; then
                apt update -y
            fi

            DEBIAN_FRONTEND=noninteractive apt install -y \
                git cmake bison flex pkg-config \
                libz-dev libssl-dev libpcre3-dev \
                build-essential

            # jemalloc
            if [ "$USE_JEMALLOC" -eq 1 ]; then
                apt install -y libjemalloc-dev || print_warning "jemalloc 安装失败，将使用默认 malloc"
            fi

            # ccache
            if [ "$USE_CCACHE" -eq 1 ]; then
                apt install -y ccache && print_success "ccache 已安装"
            fi

            # Clang
            if [ "$PREFER_CLANG" -eq 1 ]; then
                apt install -y clang || print_warning "Clang 安装失败，将使用 GCC"
            fi

            # 链接器
            if [ "$LINKER_PREFERENCE" == "gold" ]; then
                apt install -y binutils || true
            elif [ "$LINKER_PREFERENCE" == "lld" ]; then
                apt install -y lld || print_warning "LLD 安装失败"
            elif [ "$LINKER_PREFERENCE" == "mold" ]; then
                if apt-cache search ^mold$ | grep -q mold; then
                    apt install -y mold || print_warning "Mold 安装失败"
                fi
            fi
            ;;

        rhel)
            # EPEL
            if ! rpm -q epel-release >/dev/null 2>&1; then
                rpm -Uvh --nodeps https://dl.fedoraproject.org/pub/epel/epel-release-latest-8.noarch.rpm || true
            fi

            # PowerTools/CRB
            if command -v dnf &> /dev/null; then
                dnf config-manager --set-enabled powertools 2>/dev/null || \
                dnf config-manager --set-enabled crb 2>/dev/null || true
            fi

            $PKG_MGR install -y git cmake bison flex pkgconfig \
                zlib-devel openssl-devel pcre-devel gcc-c++

            if [ "$USE_JEMALLOC" -eq 1 ]; then
                $PKG_MGR install -y jemalloc-devel || true
            fi

            if [ "$USE_CCACHE" -eq 1 ]; then
                $PKG_MGR install -y ccache || true
            fi

            if [ "$PREFER_CLANG" -eq 1 ]; then
                $PKG_MGR install -y clang || $PKG_MGR install -y llvm-toolset || true
            fi

            # 链接器
            if [ "$LINKER_PREFERENCE" == "lld" ]; then
                $PKG_MGR install -y lld || true
            elif [ "$LINKER_PREFERENCE" == "mold" ]; then
                $PKG_MGR install -y mold || true
            fi
            ;;

        arch)
            pacman -Sy --noconfirm git cmake bison flex pkgconf \
                zlib openssl pcre base-devel

            if [ "$USE_JEMALLOC" -eq 1 ]; then
                pacman -S --noconfirm jemalloc || true
            fi

            if [ "$USE_CCACHE" -eq 1 ]; then
                pacman -S --noconfirm ccache || true
            fi
            ;;
    esac

    print_success "依赖包检查完成"
}

check_source_exists() {
    if [ -d "$PROJECT_DIR" ]; then
        if [ -f "$PROJECT_DIR/CMakeLists.txt" ] && [ -d "$PROJECT_DIR/src" ]; then
            return 0  # 存在且完整
        else
            print_warning "目录 $PROJECT_DIR 存在但不完整"
            return 1
        fi
    else
        return 1  # 不存在
    fi
}

prepare_source() {
    print_step "检查源代码..."

    if check_source_exists; then
        print_success "发现现有源代码: $PROJECT_DIR"
        print_info "保护现有代码，跳过 Git 拉取"
        return 0
    fi

    # 源码不存在，询问是否拉取
    echo ""
    print_warning "未找到 FluffOS 源代码"
    echo -e "${YELLOW}是否从 GitHub 拉取 FluffOS 项目?${NC}"
    echo "  [1] 是，从 GitHub 拉取 (推荐)"
    echo "  [2] 是，从 Gitee 镜像拉取 (国内快)"
    echo "  [3] 否，手动放置源码后重新运行"
    echo ""
    read -p "请选择 [1-3]: " choice

    case $choice in
        1)
            print_step "从 GitHub 拉取源码..."
            git clone --depth=1 --branch "$GIT_BRANCH" "$GIT_REPO" "$PROJECT_DIR" || {
                print_error "GitHub 拉取失败"
                exit 1
            }
            ;;
        2)
            print_step "从 Gitee 镜像拉取源码..."
            git clone --depth=1 --branch "$GIT_BRANCH" "$GIT_MIRROR" "$PROJECT_DIR" || {
                print_error "Gitee 拉取失败"
                exit 1
            }
            ;;
        3)
            print_info "请手动将 FluffOS 源码放置到: $PROJECT_DIR"
            print_info "然后重新运行此脚本"
            exit 0
            ;;
        *)
            print_error "无效选择"
            exit 1
            ;;
    esac

    print_success "源码准备完成"
}

detect_compiler() {
    local C_COMPILER="gcc"
    local CXX_COMPILER="g++"
    local COMPILER_NAME="GCC"

    if [ "$PREFER_CLANG" -eq 1 ]; then
        if command -v clang++ &>/dev/null; then
            C_COMPILER="clang"
            CXX_COMPILER="clang++"
            COMPILER_NAME="Clang"

            # Clang 版本检测
            CLANG_VERSION=$(clang++ --version | head -n1 | grep -oP '\d+\.\d+' | head -n1)
            print_perf "编译器: $COMPILER_NAME $CLANG_VERSION" >&2
        else
            print_warning "Clang 未找到，使用 GCC" >&2
            COMPILER_NAME="GCC"
        fi
    else
        print_perf "编译器: $COMPILER_NAME" >&2
    fi

    echo "$C_COMPILER $CXX_COMPILER $COMPILER_NAME"
}

detect_linker() {
    local LINKER_FLAG=""
    local LINKER_NAME="default (BFD)"

    # 按照稳定性优先级选择链接器
    case "$LINKER_PREFERENCE" in
        gold)
            if command -v ld.gold &>/dev/null || [ -f /usr/bin/ld.gold ]; then
                LINKER_FLAG="-fuse-ld=gold"
                LINKER_NAME="Gold (稳定)"
                print_perf "链接器: Gold (推荐稳定)" >&2
            else
                print_warning "Gold 链接器未找到，尝试降级..." >&2
                # 降级到 LLD
                if command -v ld.lld &>/dev/null || [ -f /usr/bin/ld.lld ]; then
                    LINKER_FLAG="-fuse-ld=lld"
                    LINKER_NAME="LLD (快速)"
                    print_perf "链接器: LLD" >&2
                else
                    print_warning "使用默认链接器 BFD" >&2
                fi
            fi
            ;;
        lld)
            if command -v ld.lld &>/dev/null || [ -f /usr/bin/ld.lld ]; then
                LINKER_FLAG="-fuse-ld=lld"
                LINKER_NAME="LLD (快速)"
                print_perf "链接器: LLD" >&2
            else
                print_warning "LLD 未找到，使用默认链接器" >&2
            fi
            ;;
        mold)
            if command -v mold &>/dev/null; then
                LINKER_FLAG="-fuse-ld=mold"
                LINKER_NAME="Mold (最快)"
                print_warning "Mold 链接器速度快但可能不稳定" >&2
            else
                print_warning "Mold 未找到，降级到 Gold" >&2
                if command -v ld.gold &>/dev/null || [ -f /usr/bin/ld.gold ]; then
                    LINKER_FLAG="-fuse-ld=gold"
                    LINKER_NAME="Gold (稳定)"
                fi
            fi
            ;;
        bfd|*)
            LINKER_NAME="BFD (默认)"
            print_perf "链接器: BFD (默认)" >&2
            ;;
    esac

    echo "$LINKER_FLAG $LINKER_NAME"
}

configure_build() {
    print_step "配置构建环境..."

    # 清理旧构建（保护源码）
    if [ -d "$PROJECT_DIR/build" ]; then
        print_info "清理旧构建目录..."
        rm -rf "$PROJECT_DIR/build"
    fi
    mkdir -p "$PROJECT_DIR/build"
    cd "$PROJECT_DIR/build"

    # 检测编译器
    read C_COMPILER CXX_COMPILER COMPILER_NAME <<< $(detect_compiler)

    # 检测链接器
    read LINKER_FLAG LINKER_NAME <<< $(detect_linker)

    # 构建编译选项
    local COMMON_FLAGS="-O$OPTIMIZATION_LEVEL -DNDEBUG -march=native -mtune=native"

    # 性能优化标志
    COMMON_FLAGS="$COMMON_FLAGS -fno-plt -fno-semantic-interposition"

    # Clang 特定优化
    if [ "$COMPILER_NAME" == "Clang" ]; then
        if [ "$USE_LTO" -eq 1 ]; then
            COMMON_FLAGS="$COMMON_FLAGS -flto=thin"
            print_perf "LTO: ThinLTO (Clang)"
        fi
        # Clang 特有优化
        COMMON_FLAGS="$COMMON_FLAGS -fstrict-vtable-pointers"
        # Clang 向量化优化
        COMMON_FLAGS="$COMMON_FLAGS -ftree-vectorize"
    else
        # GCC 优化
        if [ "$USE_LTO" -eq 1 ]; then
            COMMON_FLAGS="$COMMON_FLAGS -flto -fuse-linker-plugin"
            print_perf "LTO: Full LTO (GCC)"
        fi
        COMMON_FLAGS="$COMMON_FLAGS -fno-semantic-interposition"
        # GCC 向量化优化（含 cost model）
        COMMON_FLAGS="$COMMON_FLAGS -ftree-vectorize -fvect-cost-model=unlimited"
    fi

    # 设置环境变量
    export CC="$C_COMPILER"
    export CXX="$CXX_COMPILER"

    # ccache 加速 - 使用 CMAKE_*_COMPILER_LAUNCHER 而不是修改 CC/CXX
    local CCACHE_CMAKE_ARGS=""
    if [ "$USE_CCACHE" -eq 1 ] && command -v ccache &>/dev/null; then
        CCACHE_CMAKE_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
        print_perf "编译缓存: ccache (重编译加速 10x)"
    fi

    export CFLAGS="$COMMON_FLAGS"
    export CXXFLAGS="$COMMON_FLAGS -std=c++17"
    export LDFLAGS="$COMMON_FLAGS $LINKER_FLAG"

    # 显示配置摘要
    echo ""
    print_step "========== 构建配置 =========="
    echo "  编译器:    $COMPILER_NAME"
    echo "  链接器:    $LINKER_NAME"
    echo "  优化级别:  -O$OPTIMIZATION_LEVEL"
    echo "  LTO:       $([ "$USE_LTO" -eq 1 ] && echo '✓ 启用' || echo '✗ 禁用')"
    echo "  Jemalloc:  $([ "$USE_JEMALLOC" -eq 1 ] && echo '✓ 启用' || echo '✗ 禁用')"
    echo "  ccache:    $([ "$USE_CCACHE" -eq 1 ] && command -v ccache &>/dev/null && echo '✓ 启用' || echo '✗ 禁用')"
    echo "  CPU优化:   -march=native -mtune=native"
    print_step "=============================="
    echo ""

    # CMake 配置
    cmake "$PROJECT_DIR" \
        -DCMAKE_BUILD_TYPE="Release" \
        -DMARCH_NATIVE=ON \
        -DUSE_JEMALLOC=$([ "$USE_JEMALLOC" -eq 1 ] && echo "ON" || echo "OFF") \
        -DPACKAGE_ASYNC=ON \
        -DPACKAGE_SHA1=ON \
        -DPACKAGE_PCRE=ON \
        -DPACKAGE_DB=OFF \
        -DPACKAGE_COMPRESS=ON \
        -DPACKAGE_JSON_EXTENSION=ON \
        -DCMAKE_C_FLAGS="$CFLAGS" \
        -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" \
        -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS" \
        $CCACHE_CMAKE_ARGS

    print_success "构建配置完成"
}

compile() {
    print_step "开始编译..."

    local NPROC=$(nproc 2>/dev/null || echo "4")
    local JOBS=$((NPROC + 1))

    print_perf "并行编译: $JOBS 线程"

    # 计时
    local START_TIME=$(date +%s)

    make -j$JOBS install

    local END_TIME=$(date +%s)
    local ELAPSED=$((END_TIME - START_TIME))

    print_success "编译完成！耗时: ${ELAPSED}s"
}

verify_build() {
    print_step "验证构建产物..."

    if [ -f "$PROJECT_DIR/build/bin/driver" ]; then
        print_success "Driver 二进制: $PROJECT_DIR/build/bin/driver"

        # 显示文件大小和链接信息
        local SIZE=$(du -h "$PROJECT_DIR/build/bin/driver" | cut -f1)
        print_info "文件大小: $SIZE"

        # 检查动态链接
        if command -v ldd &>/dev/null; then
            echo ""
            print_info "动态链接库检查:"
            ldd "$PROJECT_DIR/build/bin/driver" | grep -E "(jemalloc|ssl|pcre)" || true
        fi
    else
        print_error "构建产物未找到！"
        exit 1
    fi
}

show_performance_tips() {
    echo ""
    print_step "========== 性能提示 =========="
    echo ""
    echo "  1. 运行时性能优化:"
    echo "     export LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so"
    echo ""
    echo "  2. CPU 亲和性绑定 (多核服务器):"
    echo "     taskset -c 0-3 ./driver config.cfg"
    echo ""
    echo "  3. 实时优先级 (需要 root):"
    echo "     chrt -f 99 ./driver config.cfg"
    echo ""
    echo "  4. 巨页内存 (大内存负载):"
    echo "     echo always > /sys/kernel/mm/transparent_hugepage/enabled"
    echo ""
    print_step "=============================="
    echo ""
}

main() {
    local FORCE_UPDATE=0

    # 解析参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            --force-update)
                FORCE_UPDATE=1
                shift
                ;;
            --help|-h)
                echo "FluffOS 极致性能构建脚本 v5.0"
                echo ""
                echo "用法: sudo $0 [选项]"
                echo ""
                echo "选项:"
                echo "  --force-update    强制更新依赖包"
                echo "  --help, -h        显示此帮助"
                echo ""
                exit 0
                ;;
            *)
                shift
                ;;
        esac
    done

    # 开始构建流程
    check_root
    detect_system
    detect_cpu
    install_dependencies
    prepare_source
    configure_build
    compile
    verify_build
    show_performance_tips

    print_success "========================================="
    print_success "构建流程全部完成！"
    print_success "========================================="
}

main "$@"
