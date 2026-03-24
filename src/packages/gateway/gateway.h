/*
 * Gateway Package - 内网版头文件 v2.6
 * Go 网关与 FluffOS 通信，使用 interactive_t 虚拟对象
 *
 * 内网环境，无需复杂安全验证
 * 核心功能: 连接管理、心跳检测、会话管理
 *
 * v2.6 重构：
 * - 移除 RPC 回调系统（简化设计，回调逻辑移至 LPC 层）
 * - 保留核心 IO 功能和会话管理
 * - 优化消息路由，由 LPC 层处理业务逻辑
 */

#ifndef PACKAGES_GATEWAY_H
#define PACKAGES_GATEWAY_H

#include "base/package_api.h"
#include "packages/json_extension/yyjson.h"

#include <string>
#include <unordered_map>
#include <cstdint>

// ============================================================
// 配置常量
// ============================================================

#define GATEWAY_DEFAULT_MAX_PACKET_SIZE (16 * 1024 * 1024)  // 16MB
#define GATEWAY_HEADER_SIZE 4                  // 4字节长度头 (BigEndian)
#define GATEWAY_DEFAULT_MAX_MASTERS 10         // 默认最大主连接数
#define GATEWAY_DEFAULT_MAX_SESSIONS 5000      // 默认最大会话数
#define GATEWAY_DEFAULT_HEARTBEAT_INTERVAL 5   // 默认心跳间隔(秒)
#define GATEWAY_DEFAULT_HEARTBEAT_TIMEOUT 300  // 默认心跳超时(秒) - 增加到5分钟以支持高并发

// ============================================================
// 全局配置（可通过 gateway_config efun 修改）
// ============================================================

extern int g_gateway_debug;
extern size_t g_gateway_max_packet_size;
extern int g_gateway_max_masters;
extern int g_gateway_max_sessions;
extern int g_gateway_heartbeat_interval;
extern int g_gateway_heartbeat_timeout;

#define GW_DEBUG(...) if (g_gateway_debug) { debug_message(__VA_ARGS__); }
#define GW_LOG(...) debug_message(__VA_ARGS__)

// ============================================================
// Efun 声明
// ============================================================

// 核心 apply (宏定义由 build_applies 从 applies 文件生成)
// #define APPLY_GATEWAY_CONNECT gateway_connect

// 核心功能
void f_gateway_listen(void);
void f_gateway_send(void);
void f_gateway_status(void);
void f_gateway_config(void);

// 会话管理
void f_gateway_session_send(void);
void f_gateway_destroy_session(void);
void f_gateway_sessions(void);
void f_gateway_session_info(void);

// 工具函数
void f_is_gateway_user(void);

// 心跳管理
void f_gateway_set_heartbeat(void);    // 设置心跳间隔
void f_gateway_check_timeout(void);    // 检查超时连接
void f_gateway_ping_master(void);      // ping指定主连接

// ============================================================
// 初始化和清理
// ============================================================

void init_gateway(void);
void cleanup_gateway(void);

// ============================================================
// 内部接口 - 供 comm.cc 调用
// ============================================================

/**
 * 通过 session_id 发送数据到 Gateway
 */
int gateway_send_to_session(const char* session_id, const char* data, size_t len);

/**
 * 通过 fd 发送原始数据（带帧头）
 * @param fd 主连接 fd
 * @param data 数据内容
 * @param len 数据长度
 */
int gateway_send_raw_to_fd(int fd, const char* data, size_t len);

/**
 * 通过 fd 发送原始数据（带帧头），并接管 data 的释放责任（free）
 * 适用于 yyjson_mut_write / build_gateway_output_packet 返回的堆内存，
 * 避免额外数据拷贝。
 */
int gateway_send_raw_to_fd_owned(int fd, char* data, size_t len);

/**
 * 调用 Gateway 的发送完成回调
 */
void gateway_trigger_write_callback(int fd);

/**
 * 检查对象是否是 Gateway 会话
 */
bool gateway_is_session(struct object_t* ob);

/**
 * 获取 Gateway 会话的真实 IP
 */
const char* gateway_get_ip(struct object_t* ob);

// ============================================================
// 内部接口 - 会话管理
// ============================================================

/**
 * 创建会话（内部接口）
 * data_val: Go 发送的完整 login data，会传递给 master->gateway_connect(data)
 */
struct object_t* gateway_create_session_internal(int master_fd, const char* session_id,
                                                  struct svalue_t* data_val,
                                                  const char* ip, int port);

/**
 * 销毁会话（内部接口）
 */
int gateway_destroy_session_internal(const char* session_id, const char* reason_code,
                                     const char* reason_text);

/**
 * 通过 session_id 查找用户对象
 */
struct object_t* gateway_find_user_by_session(const char* session_id);

/**
 * 向用户注入输入
 */
int gateway_inject_input_internal(struct object_t* user, const char* input);

/**
 * exec() 时更新会话映射
 */
void gateway_session_exec_update(struct object_t* new_ob, struct object_t* old_ob);

/**
 * 用户断开连接时的清理钩子
 */
void gateway_handle_remove_interactive(struct interactive_t* ip);

/**
 * 清理指定主连接的所有会话
 */
void gateway_cleanup_master_sessions(int master_fd);

/**
 * 获取会话数量
 */
int gateway_get_session_count(void);

/**
 * 获取主连接数量
 */
int gateway_get_master_count(void);

/**
 * 检查主连接心跳是否超时
 */
void gateway_check_heartbeat_timeouts(void);

/**
 * 发送ping到主连接
 */
int gateway_ping_master(int master_fd);

/**
 * 检查会话超时
 */
void gateway_check_session_timeouts(void);

// ============================================================
// JSON 转换函数（从 pkg_json.cc 导入）
// ============================================================

extern void json_to_svalue(yyjson_val* val, svalue_t* out, int depth);

/**
 * 将 svalue_t 转换为 JSON 字符串
 */
int svalue_to_json_string(svalue_t* sv, char** out_json_str, size_t* out_len);

/**
 * 构建 Gateway Output 消息包 (C Linkage)
 * 用于 gateway_send_to_session() 系统内部文本消息发送
 */
extern "C" int build_gateway_output_packet(const char* cid, const char* data, char** out_json_str, size_t* out_len);

// ============================================================
// RAII 包装类 - 自动管理内存，防止 longjmp 导致的内存泄漏
// ============================================================

/**
 * JsonBuffer - RAII 包装类，自动管理 JSON 缓冲区内存
 *
 * 功能：
 * - 构造时分配内存
 * - 析构时自动释放内存（即使 LPC 触发 longjmp）
 * - 禁止拷贝，防止重复释放
 * - 支持移动语义（可选优化）
 */
class JsonBuffer {
private:
    char* data_;
    size_t len_;

public:
    /**
     * 构造函数：分配指定大小的内存
     * @param len 缓冲区大小（字节）
     */
    explicit JsonBuffer(size_t len) : data_(nullptr), len_(len) {
        if (len > 0) {
            data_ = new char[len + 1];
            data_[len] = '\0';  // 确保 null 结尾
        }
    }

    /**
     * 析构函数：自动释放内存 ⭐ 关键特性
     * 无论是否发生 longjmp，都会执行
     */
    ~JsonBuffer() {
        delete[] data_;
    }

    // 禁止拷贝构造和拷贝赋值（防止重复释放）
    JsonBuffer(const JsonBuffer&) = delete;
    JsonBuffer& operator=(const JsonBuffer&) = delete;

    /**
     * 移动构造函数（可选，性能优化）
     */
    JsonBuffer(JsonBuffer&& other) noexcept
        : data_(other.data_), len_(other.len_) {
        other.data_ = nullptr;
        other.len_ = 0;
    }

    /**
     * 移动赋值运算符（可选）
     */
    JsonBuffer& operator=(JsonBuffer&& other) noexcept {
        if (this != &other) {
            delete[] data_;
            data_ = other.data_;
            len_ = other.len_;
            other.data_ = nullptr;
            other.len_ = 0;
        }
        return *this;
    }

    // 访问器
    char* data() { return data_; }
    const char* data() const { return data_; }
    size_t size() const { return len_; }

    // 有效性检查
    bool is_valid() const { return data_ != nullptr; }
};

/**
 * JsonDocGuard - RAII 包装类，自动管理 yyjson_doc
 *
 * 功能：
 * - 构造时接收 yyjson_doc 指针
 * - 析构时自动调用 yyjson_doc_free()（即使 LPC 触发 longjmp）
 * - 禁止拷贝，防止重复释放
 */
class JsonDocGuard {
private:
    yyjson_doc* doc_;

public:
    /**
     * 构造函数：接收 yyjson_doc 指针
     * @param doc yyjson 文档指针（可以为 nullptr）
     */
    explicit JsonDocGuard(yyjson_doc* doc) : doc_(doc) {}

    /**
     * 析构函数：自动释放 JSON 文档 ⭐ 关键特性
     * 无论是否发生 longjmp，都会执行
     */
    ~JsonDocGuard() {
        if (doc_) {
            yyjson_doc_free(doc_);
            doc_ = nullptr;
        }
    }

    // 禁止拷贝构造和拷贝赋值（防止重复释放）
    JsonDocGuard(const JsonDocGuard&) = delete;
    JsonDocGuard& operator=(const JsonDocGuard&) = delete;

    /**
     * 获取 JSON 文档的根节点
     * @return 根节点指针，如果文档无效则返回 nullptr
     */
    yyjson_val* root() {
        return doc_ ? yyjson_doc_get_root(doc_) : nullptr;
    }

    /**
     * 获取 JSON 文档的根节点（const 版本）
     */
    const yyjson_val* root() const {
        return doc_ ? yyjson_doc_get_root(doc_) : nullptr;
    }

    /**
     * 有效性检查
     * @return 如果文档有效返回 true
     */
    bool is_valid() const { return doc_ != nullptr; }
};

#endif // PACKAGES_GATEWAY_H
