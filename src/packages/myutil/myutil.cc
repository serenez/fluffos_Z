/*
 * MyUtil Package - 实用工具集 v1.2 (高性能自动清理版)
 *
 * v1.2 改进:
 * - 采用“惰性清理”替代“全量扫描”，消除性能抖动
 * - 引入“活跃保活”机制，add 操作会刷新 TTL
 * - 修复 string_print_formatted 内存释放方式
 */

#include "base/package_api.h"
#include "myutil.h"

#include "vm/vm.h"

#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <ctime>

// ============================================================
// UUID 生成器 (保持不变)
// ============================================================
static std::random_device g_random_device;
static std::mt19937_64 g_uuid_gen(g_random_device());
static std::uniform_int_distribution<uint64_t> g_uuid_dis;

std::string generate_uuid_v4() {
    uint64_t p1 = g_uuid_dis(g_uuid_gen);
    uint64_t p2 = g_uuid_dis(g_uuid_gen);
    unsigned char bytes[16];
    memcpy(&bytes[0], &p1, 8);
    memcpy(&bytes[8], &p2, 8);
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;
    
    char uuid_str[37];
    const char *hex = "0123456789abcdef";
    int i = 0, j = 0;
    for(; i<4; i++) { uuid_str[j++] = hex[bytes[i]>>4]; uuid_str[j++] = hex[bytes[i]&0xF]; }
    uuid_str[j++] = '-';
    for(; i<6; i++) { uuid_str[j++] = hex[bytes[i]>>4]; uuid_str[j++] = hex[bytes[i]&0xF]; }
    uuid_str[j++] = '-';
    for(; i<8; i++) { uuid_str[j++] = hex[bytes[i]>>4]; uuid_str[j++] = hex[bytes[i]&0xF]; }
    uuid_str[j++] = '-';
    for(; i<10; i++) { uuid_str[j++] = hex[bytes[i]>>4]; uuid_str[j++] = hex[bytes[i]&0xF]; }
    uuid_str[j++] = '-';
    for(; i<16; i++) { uuid_str[j++] = hex[bytes[i]>>4]; uuid_str[j++] = hex[bytes[i]&0xF]; }
    uuid_str[36] = '\0';
    return std::string(uuid_str);
}

void f_uuid(void) {
    std::string uuid = generate_uuid_v4();
    copy_and_push_string(uuid.c_str());
}

// ============================================================
// StringBuffer 全局状态 (升级版)
// ============================================================

#include "packages/core/sprintf.h"

struct StrBuf {
    std::string buffer;
    time_t last_touch; // 改名为 last_touch，表示最后活跃时间
};

struct ExpirationRecord {
    int64_t id;
    time_t expires_at;
};

static std::unordered_map<int64_t, StrBuf> g_str_buffers;
static std::deque<ExpirationRecord> g_expiration_queue;
static int64_t g_str_buf_id_counter = 0;

// 配置
const size_t MAX_STR_BUFFERS = 100000;  // 最大容量
const int STR_BUFFER_TTL = 60;        // 超时时间 (60秒)
const int CLEANUP_BATCH_SIZE = 10;    // 每次只检查 10 个

// ------------------------------------------------------------
// 内部辅助：惰性清理
// ------------------------------------------------------------
static inline void touch_str_buffer(int64_t id, StrBuf& sb, time_t now) {
    sb.last_touch = now;
    g_expiration_queue.push_back({id, now + STR_BUFFER_TTL});
}

static void perform_lazy_cleanup() {
    if (g_str_buffers.empty() || g_expiration_queue.empty()) return;

    time_t now = time(nullptr);
    int checked = 0;

    // 过期队列按访问时间递增推进，旧记录即使重复出现也能被快速跳过。
    while (!g_expiration_queue.empty() && checked < CLEANUP_BATCH_SIZE) {
        auto record = g_expiration_queue.front();
        if (record.expires_at > now) {
            break;
        }
        g_expiration_queue.pop_front();
        auto it = g_str_buffers.find(record.id);
        if (it != g_str_buffers.end() && (it->second.last_touch + STR_BUFFER_TTL) <= now) {
            g_str_buffers.erase(it);
        }
        checked++;
    }
}

// ============================================================
// Efun 实现
// ============================================================

/**
 * strbuf_new() - 创建新的字符串缓冲区
 */
void f_strbuf_new(void) {
    // 1. 每次创建前，顺手清理几个旧垃圾 (分摊开销)
    perform_lazy_cleanup();

    // 2. 熔断机制：如果清理后依然满员，拒绝创建
    if (g_str_buffers.size() >= MAX_STR_BUFFERS) {
        // 返回 0 表示创建失败
        push_number(0);
        return;
    }

    int64_t id = ++g_str_buf_id_counter;
    if (id <= 0) id = g_str_buf_id_counter = 1;
    
    StrBuf& sb = g_str_buffers[id];
    sb.buffer.reserve(256);
    touch_str_buffer(id, sb, time(nullptr));
    
    push_number(id);
}

/**
 * strbuf_add(int buf, string text)
 */
void f_strbuf_add(void) {
    int64_t id = (sp - 1)->u.number;
    const char* text = sp->u.string;
    perform_lazy_cleanup();
    
    auto it = g_str_buffers.find(id);
    if (it != g_str_buffers.end()) {
        it->second.buffer.append(text);
        touch_str_buffer(id, it->second, time(nullptr));
    }
    
    pop_stack(); // pop text
    pop_stack(); // pop id
}

/**
 * strbuf_addf(int buf, string fmt, ...)
 */
void f_strbuf_addf(void) {
    int num_arg = st_num_arg;
    int64_t id = (sp - num_arg + 1)->u.number;
    const char* fmt = (sp - num_arg + 2)->u.string;
    perform_lazy_cleanup();
    
    auto it = g_str_buffers.find(id);
    if (it == g_str_buffers.end()) {
        pop_n_elems(num_arg);
        return;
    }
    
    char* res = string_print_formatted(fmt, num_arg - 2, sp - num_arg + 3);
    
    if (res) {
        it->second.buffer.append(res);
        touch_str_buffer(id, it->second, time(nullptr));
        // 注意：标准 FluffOS 中 string_print_formatted 返回 malloc 内存
        // 建议使用 free() 而不是 FREE() 宏，除非你确定 FREE 映射到了 free
        free(res); 
    }
    
    pop_n_elems(num_arg);
}

/**
 * strbuf_dump(int buf) - 导出并销毁
 */
void f_strbuf_dump(void) {
    int64_t id = sp->u.number;
    pop_stack();
    
    auto it = g_str_buffers.find(id);
    if (it != g_str_buffers.end()) {
        copy_and_push_string(it->second.buffer.c_str());
        g_str_buffers.erase(it); // 正常销毁
    } else {
        push_undefined();
    }
}

/**
 * strbuf_clear(int buf) - 手动销毁不导出 (新增接口)
 */
void f_strbuf_clear(void) {
    int64_t id = sp->u.number;
    pop_stack();
    
    g_str_buffers.erase(id);
}

// ... init/cleanup ...
void init_myutil(void) { g_uuid_dis(g_uuid_gen); }
void cleanup_myutil(void) {}
