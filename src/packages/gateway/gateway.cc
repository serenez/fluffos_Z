/*
 * Gateway Package - 内网版主文件 v2.7 (Fixes Memory Corruption)
 * TCP 连接管理 + 协议处理 + 心跳检测
 *
 * v2.7 修复 (2026-01-08):
 * - FIX: 修复 YYJSON_READ_INSITU 导致的 evbuffer 内存损坏
 * - FIX: 先 drain 再调用 LPC，避免 longjmp 导致缓冲区状态错误
 */

#include "base/std.h"
#include "gateway.h"
#include "backend.h"
#include "comm.h"

#include "vm/vm.h"
#include "vm/internal/base/svalue.h"
#include "vm/internal/base/array.h"
#include "vm/internal/base/mapping.h"
#include "vm/internal/base/machine.h"
#include "vm/internal/base/interpret.h"
#include "base/internal/stralloc.h"
#include "base/internal/rc.h"

#include <random>

// 导入 pkg_json 的函数
extern int svalue_to_json_string(svalue_t* sv, char** out_json_str, size_t* out_len);

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>

#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <cstring>
#include <ctime>
#include <chrono>
#include <atomic>
#include <vector>

// ============================================================
// 全局配置
// ============================================================

int g_gateway_debug = 0;
size_t g_gateway_max_packet_size = GATEWAY_DEFAULT_MAX_PACKET_SIZE;
int g_gateway_max_masters = GATEWAY_DEFAULT_MAX_MASTERS;
int g_gateway_max_sessions = GATEWAY_DEFAULT_MAX_SESSIONS;
int g_gateway_heartbeat_interval = GATEWAY_DEFAULT_HEARTBEAT_INTERVAL;
int g_gateway_heartbeat_timeout = GATEWAY_DEFAULT_HEARTBEAT_TIMEOUT;

// ============================================================
// 循环引用检测器
// ============================================================

class CircularChecker {
private:
    std::vector<void*> stack_;
    std::unordered_set<void*> deep_set_;

public:
    CircularChecker() { stack_.reserve(64); }

    inline bool contains(void* ptr, int depth) const {
        for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
            if (*it == ptr) return true;
        }
        if (depth >= 16) {
            return deep_set_.find(ptr) != deep_set_.end();
        }
        return false;
    }

    inline void insert(void* ptr, int depth) {
        stack_.push_back(ptr);
        if (depth >= 16) deep_set_.insert(ptr);
    }

    inline void remove(void* ptr, int depth) {
        stack_.pop_back();
        if (depth >= 16) deep_set_.erase(ptr);
    }
};

// ============================================================
// 对象有效性验证辅助函数
// ============================================================

static inline bool is_object_valid(object_t* ob) {
    if (!ob) return false;
    if (ob->flags & O_DESTRUCTED) return false;
    if (!ob->obname || ob->obname[0] == '\0') return false;
    return true;
}

// ============================================================
// 统计信息
// ============================================================

static struct {
    uint64_t total_connections;
    uint64_t messages_received;
    uint64_t messages_sent;
    uint64_t bytes_received;
    uint64_t bytes_sent;
    time_t started_at;
} g_stats = {0};

extern struct event_base *g_event_base;
static struct event *g_heartbeat_timer = nullptr;

// ============================================================
// 协议处理函数前向声明
// ============================================================

static void handle_hello(int fd, yyjson_val* msg);
static void handle_login(int fd, yyjson_val* msg);
static void handle_data(int fd, yyjson_val* msg);
static void handle_discon(int fd, yyjson_val* msg);
static void handle_sys(int fd, yyjson_val* msg);

// ============================================================
// 主连接结构
// ============================================================

struct GatewayMaster {
    int fd;
    struct bufferevent *bev;
    std::string ip;
    bool marked_for_delete;
    time_t connected_at;
    time_t last_heartbeat;
    uint64_t messages_received;
    uint64_t messages_sent;

    GatewayMaster()
        : fd(-1), bev(nullptr), marked_for_delete(false),
          connected_at(0), last_heartbeat(0),
          messages_received(0), messages_sent(0) {
        connected_at = time(nullptr);
        last_heartbeat = connected_at;
    }

    ~GatewayMaster() {
        if (bev) {
            bufferevent_setcb(bev, nullptr, nullptr, nullptr, nullptr);
            bufferevent_free(bev);
            bev = nullptr;
        }
    }

    bool is_timeout() const {
        time_t now = get_current_time();
        return (now - last_heartbeat) > g_gateway_heartbeat_timeout;
    }

    void update_heartbeat(time_t now = 0) {
        if (now == 0) {
            now = get_current_time();
        }
        // 节流：同一秒内重复更新没有意义。
        if (now > last_heartbeat) {
            last_heartbeat = now;
        }
    }
};

static std::unordered_map<int, std::unique_ptr<GatewayMaster>> g_masters;
static struct evconnlistener *g_listener = nullptr;
static int g_next_fd = 1;

// ============================================================
// 辅助函数
// ============================================================

static int generate_safe_id() {
    int start_id = g_next_fd;
    int max_attempts = 100;

    for (int i = 0; i < max_attempts; i++) {
        int id = g_next_fd;
        unsigned int next = static_cast<unsigned int>(g_next_fd) + 1;
        if (next > 2000000000) next = 1;
        g_next_fd = static_cast<int>(next);

        if (g_masters.find(id) == g_masters.end()) return id;
        if (g_next_fd == start_id) break;
    }
    return -1;
}

// ⭐ 阶段4优化：TCP 参数深度调优
static void set_tcp_options(evutil_socket_t fd) {
    int optval = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&optval, sizeof(optval));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&optval, sizeof(optval));

    // ⭐ 阶段4优化：增大接收和发送缓冲区到 2MB
    // 默认值通常只有几十KB，不足以应对高并发突发流量
    int rcvbuf_size = 2 * 1024 * 1024;  // 2MB 接收缓冲区
    int sndbuf_size = 2 * 1024 * 1024;  // 2MB 发送缓冲区
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf_size, sizeof(rcvbuf_size));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf_size, sizeof(sndbuf_size));

    // 调试日志（验证设置是否生效）
    int actual_rcvbuf = 0, actual_sndbuf = 0;
    socklen_t optlen = sizeof(int);
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char*)&actual_rcvbuf, &optlen);
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char*)&actual_sndbuf, &optlen);
    GW_LOG("[Gateway] fd=%d: TCP buffers optimized (rcvbuf=%dMB, sndbuf=%dMB)\n",
           fd, actual_rcvbuf / (1024 * 1024), actual_sndbuf / (1024 * 1024));

#ifdef TCP_KEEPIDLE
    int idle = g_gateway_heartbeat_interval;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, (const char*)&idle, sizeof(idle));
#endif

#ifdef TCP_KEEPINTVL
    int intvl = g_gateway_heartbeat_interval / 2;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, (const char*)&intvl, sizeof(intvl));
#endif
}

// ============================================================
// JSON 转换
// ============================================================

extern int svalue_to_json_string(svalue_t* sv, char** out_json_str, size_t* out_len);
extern void json_to_svalue(yyjson_val* val, svalue_t* out, int depth);
extern yyjson_mut_val* svalue_to_json_impl(yyjson_mut_doc* doc, svalue_t* sv,
                                           CircularChecker* checker, int depth);

static void apply_gateway_receive(object_t* user, svalue_t* data_sv) {
    save_command_giver(user);
    current_interactive = user;
    set_eval(max_eval_cost);
    push_svalue(data_sv);
    safe_apply("gateway_receive", user, 1, ORIGIN_DRIVER);
    current_interactive = nullptr;
    restore_command_giver();
}

static std::vector<int> collect_master_ids() {
    std::vector<int> ids;
    ids.reserve(g_masters.size());
    for (const auto& pair : g_masters) {
        ids.push_back(pair.first);
    }
    return ids;
}

static bool extract_cmd_text_fast(yyjson_val* data, const char** out_text) {
    if (!data || !out_text) return false;
    *out_text = nullptr;

    // 兼容直接传字符串命令
    if (yyjson_is_str(data)) {
        const char* text = yyjson_get_str(data);
        if (text && text[0]) {
            *out_text = text;
            return true;
        }
        return false;
    }

    if (!yyjson_is_obj(data)) return false;

    yyjson_val* type = yyjson_obj_get(data, "type");
    if (!type || !yyjson_is_str(type) || strcmp(yyjson_get_str(type), "cmd") != 0) {
        return false;
    }

    yyjson_val* body = yyjson_obj_get(data, "data");
    if (!body) return false;

    if (yyjson_is_str(body)) {
        const char* text = yyjson_get_str(body);
        if (text && text[0]) {
            *out_text = text;
            return true;
        }
        return false;
    }

    if (yyjson_is_obj(body)) {
        yyjson_val* text = yyjson_obj_get(body, "text");
        if (text && yyjson_is_str(text)) {
            const char* cmd = yyjson_get_str(text);
            if (cmd && cmd[0]) {
                *out_text = cmd;
                return true;
            }
        }
    }

    return false;
}

// ============================================================
// 协议处理
// ============================================================

static void handle_hello(int fd, yyjson_val* msg) {
    auto it = g_masters.find(fd);
    if (it == g_masters.end()) return;

    GatewayMaster* master = it->second.get();
    yyjson_val* data = yyjson_obj_get(msg, "data");
    const char* service = (data && yyjson_is_str(data)) ? yyjson_get_str(data) : "unknown";
    time_t now = get_current_time();

    GW_LOG("[Gateway] Gateway registered: fd=%d, service=%s\n", fd, service);
    master->update_heartbeat(now);
}

static void handle_login(int fd, yyjson_val* msg) {
    auto it = g_masters.find(fd);
    if (it == g_masters.end()) return;

    GatewayMaster* master = it->second.get();
    yyjson_val* cid = yyjson_obj_get(msg, "cid");
    yyjson_val* data = yyjson_obj_get(msg, "data");

    if (!cid || !yyjson_is_str(cid)) {
        GW_LOG("[Gateway] login: missing cid\n");
        return;
    }

    const char* session_id = yyjson_get_str(cid);
    const char* ip = nullptr;
    int port = 0;

    if (data && yyjson_is_obj(data)) {
        yyjson_val* v_ip = yyjson_obj_get(data, "ip");
        yyjson_val* v_port = yyjson_obj_get(data, "port");
        if (v_ip && yyjson_is_str(v_ip)) ip = yyjson_get_str(v_ip);
        if (v_port && yyjson_is_int(v_port)) port = (int)yyjson_get_int(v_port);
    }

    GW_DEBUG("[Gateway] login: cid=%s, ip=%s, port=%d\n", session_id, ip ? ip : "null", port);
    master->update_heartbeat(get_current_time());

    svalue_t data_sv;
    if (data && yyjson_is_obj(data)) {
        json_to_svalue(data, &data_sv, 0);
    } else {
        data_sv.type = T_NUMBER;
        data_sv.u.number = 0;
    }

    object_t* ob = gateway_create_session_internal(fd, session_id, &data_sv, ip, port);
    free_svalue(&data_sv, "gateway_login");

    if (!ob) {
        GW_LOG("[Gateway] login failed for %s\n", session_id);
    } 
    // else {
    //     GW_LOG("[Gateway] login success: %s -> %s\n", session_id, ob->obname);
    // }
}

static void handle_data(int fd, yyjson_val* msg) {
    auto it = g_masters.find(fd);
    if (it == g_masters.end()) return;

    GatewayMaster* master = it->second.get();
    yyjson_val* cid = yyjson_obj_get(msg, "cid");
    yyjson_val* data = yyjson_obj_get(msg, "data");

    if (!cid || !yyjson_is_str(cid)) {
        GW_LOG("[Gateway] data: missing cid\n");
        return;
    }

    const char* session_id = yyjson_get_str(cid);
    object_t* user = gateway_find_user_by_session(session_id);
    if (!user) {
        // ⚠️ 高并发下禁用日志，避免 I/O 阻塞事件循环
        // GW_LOG("[Gateway] data: user not found for cid=%s\n", session_id);
        return;
    }

    time_t now = get_current_time();
    master->update_heartbeat(now);
    if (user->interactive) {
        user->interactive->last_time = now;
    }

    svalue_t data_sv;
    const char* cmd_text = nullptr;
    if (extract_cmd_text_fast(data, &cmd_text)) {
        data_sv.type = T_STRING;
        data_sv.subtype = STRING_MALLOC;
        data_sv.u.string = string_copy(cmd_text, "gateway_cmd_fast");
    } else if (data) {
        json_to_svalue(data, &data_sv, 0);
    } else {
        data_sv.type = T_NUMBER;
        data_sv.u.number = 0;
    }

    // ⚠️ 高并发下禁用调试日志
    // GW_DEBUG("[Gateway] data: cid=%s -> gateway_receive()\n", session_id);
    apply_gateway_receive(user, &data_sv);
    free_svalue(&data_sv, "gateway_data");
}

static void handle_discon(int fd, yyjson_val* msg) {
    auto it = g_masters.find(fd);
    if (it == g_masters.end()) return;

    GatewayMaster* master = it->second.get();
    yyjson_val* cid = yyjson_obj_get(msg, "cid");
    if (!cid || !yyjson_is_str(cid)) return;

    const char* session_id = yyjson_get_str(cid);
    yyjson_val* reason_code = yyjson_obj_get(msg, "reason_code");
    const char* reason_code_str =
        (reason_code && yyjson_is_str(reason_code)) ? yyjson_get_str(reason_code) : "client_disconnected";
    yyjson_val* reason_text = yyjson_obj_get(msg, "reason_text");
    const char* reason_text_str =
        (reason_text && yyjson_is_str(reason_text)) ? yyjson_get_str(reason_text) : nullptr;
    if (!reason_text_str || reason_text_str[0] == '\0') {
        yyjson_val* reason = yyjson_obj_get(msg, "reason");
        reason_text_str = (reason && yyjson_is_str(reason)) ? yyjson_get_str(reason) : "client disconnect";
    }

    GW_DEBUG("[Gateway] discon: cid=%s, reason_code=%s, reason_text=%s\n",
             session_id, reason_code_str, reason_text_str);
    master->update_heartbeat(get_current_time());

    // remove_interactive(ob, 0) inside gateway_destroy_session_internal()
    // already triggers APPLY_NET_DEAD once.
    gateway_destroy_session_internal(session_id, reason_code_str, reason_text_str);
}

static void handle_sys(int fd, yyjson_val* msg) {
    auto it = g_masters.find(fd);
    if (it == g_masters.end()) return;

    GatewayMaster* master = it->second.get();
    yyjson_val* action = yyjson_obj_get(msg, "action");
    const char* action_str = (action && yyjson_is_str(action)) ? yyjson_get_str(action) : "unknown";

    if (strcmp(action_str, "ping") == 0) {
        master->update_heartbeat(get_current_time());
        char pong_msg[32];
        int len = snprintf(pong_msg, sizeof(pong_msg), "{\"type\":\"sys\",\"action\":\"pong\"}");
        gateway_send_raw_to_fd(fd, pong_msg, len);
        GW_DEBUG("[Gateway] ping received from fd=%d, pong sent\n", fd);
        return;
    }

    if (strcmp(action_str, "pong") == 0) {
        master->update_heartbeat(get_current_time());
        GW_DEBUG("[Gateway] pong received from fd=%d\n", fd);
        return;
    }

    time_t now = get_current_time();
    master->update_heartbeat(now);

    yyjson_val* cid = yyjson_obj_get(msg, "cid");
    if (cid && yyjson_is_str(cid)) {
        const char* session_id = yyjson_get_str(cid);
        object_t* user = gateway_find_user_by_session(session_id);

        if (user && !(user->flags & O_DESTRUCTED)) {
            const char* cmd_text = nullptr;
            if (user->interactive) {
                user->interactive->last_time = now;
            }
            yyjson_val* data = yyjson_obj_get(msg, "data");
            svalue_t data_sv;

            if (extract_cmd_text_fast(data, &cmd_text)) {
                data_sv.type = T_STRING;
                data_sv.subtype = STRING_MALLOC;
                data_sv.u.string = string_copy(cmd_text, "gateway_cmd_fast");
            } else if (data) {
                json_to_svalue(data, &data_sv, 0);
            } else {
                data_sv.type = T_NUMBER;
                data_sv.u.number = 0;
            }

            apply_gateway_receive(user, &data_sv);
            free_svalue(&data_sv, "gateway_data");
            return;
        }
    }

    object_t* gateway_d = find_object("/adm/daemons/gateway_d");
    if (gateway_d) {
        svalue_t msg_sv;
        json_to_svalue(msg, &msg_sv, 0);

        save_command_giver(nullptr);
        set_eval(max_eval_cost);
        push_svalue(&msg_sv);
        safe_apply("receive_system_message", gateway_d, 1, ORIGIN_DRIVER);
        restore_command_giver();

        free_svalue(&msg_sv, "gateway_sys_msg");
    } else {
        GW_DEBUG("[Gateway] gateway_d not found, sys message ignored\n");
    }
}

// 消息分发：固定分支，避免每包字符串哈希/临时构造开销。
static void dispatch_message(int fd, yyjson_val* msg) {
    if (!msg || !yyjson_is_obj(msg)) return;

    g_stats.messages_received++;

    yyjson_val* type = yyjson_obj_get(msg, "type");
    if (!type || !yyjson_is_str(type)) {
        GW_DEBUG("[Gateway] Unknown message format\n");
        return;
    }

    const char* type_str = yyjson_get_str(type);
    if (strcmp(type_str, "hello") == 0) {
        handle_hello(fd, msg);
        return;
    }
    if (strcmp(type_str, "login") == 0) {
        handle_login(fd, msg);
        return;
    }
    if (strcmp(type_str, "data") == 0) {
        handle_data(fd, msg);
        return;
    }
    if (strcmp(type_str, "discon") == 0) {
        handle_discon(fd, msg);
        return;
    }
    if (strcmp(type_str, "sys") == 0) {
        handle_sys(fd, msg);
        return;
    }

    // ❌ 未知消息类型
    GW_DEBUG("[Gateway] Unknown message type: %s\n", type_str);
}

// ============================================================
// 主连接管理
// ============================================================

static void remove_master(int fd, const char* reason) {
    auto it = g_masters.find(fd);
    if (it == g_masters.end()) return;

    GatewayMaster* master = it->second.get();
    if (master->marked_for_delete) return;

    master->marked_for_delete = true;

    GW_LOG("[Gateway] Master disconnected: fd=%d, ip=%s, reason=%s, "
           "msgs_in=%llu, msgs_out=%llu\n",
           fd, master->ip.c_str(), reason,
           (unsigned long long)master->messages_received,
           (unsigned long long)master->messages_sent);

    gateway_cleanup_master_sessions(fd);
    g_masters.erase(it);
}

// ⭐ 阶段3优化：批量处理消息，减少回调次数（每次最多处理200条）
// ⭐ 优化：批量大小从 50 增加到 200，减少回调次数 4 倍
static void conn_readcb(struct bufferevent *bev, void *ctx) {
    GatewayMaster *master = (GatewayMaster *)ctx;
    int master_fd = master->fd;
    struct evbuffer *input = bufferevent_get_input(bev);

    // ⭐ 批量处理循环：每次最多处理200条消息
    int batch_count = 0;
    const int MAX_BATCH_SIZE = 200;

    while (batch_count < MAX_BATCH_SIZE) {
        size_t len = evbuffer_get_length(input);
        if (len < GATEWAY_HEADER_SIZE) break;  // 没有足够的数据，退出

        // Read message length header (peek only, don't drain yet)
        uint32_t net_len;
        evbuffer_copyout(input, &net_len, GATEWAY_HEADER_SIZE);
        uint32_t msg_len = ntohl(net_len);

        // Validate message length before any buffer operations
        if (msg_len > g_gateway_max_packet_size) {
            GW_LOG("[Gateway][ERROR] Message too large: %u bytes from fd=%d\n", msg_len, master->fd);
            GW_LOG("[Gateway][ERROR] Buffer state: len=%zu, first 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   len, net_len & 0xFF, (net_len >> 8) & 0xFF, (net_len >> 16) & 0xFF, (net_len >> 24) & 0xFF);
            remove_master(master->fd, "message too large");
            return;
        }

        if (msg_len == 0) {
            GW_LOG("[Gateway][ERROR] Zero length message, draining header\n");
            evbuffer_drain(input, GATEWAY_HEADER_SIZE);
            continue;  // 继续处理下一条消息
        }

        // Check if complete message is available
        if (len < GATEWAY_HEADER_SIZE + msg_len) {
            // Incomplete message, wait for more data
            break;  // 退出批量处理
        }

        // Pull up message data (peek only, don't drain yet)
        unsigned char *data_ptr = evbuffer_pullup(input, GATEWAY_HEADER_SIZE + msg_len);
        if (!data_ptr) {
            GW_LOG("[Gateway][ERROR] Failed to pullup %u bytes from buffer\n", GATEWAY_HEADER_SIZE + msg_len);
            remove_master(master->fd, "pullup failed");
            return;
        }

        // Advance data_ptr past the header
        data_ptr += GATEWAY_HEADER_SIZE;

        g_stats.bytes_received += msg_len;

        // Parse JSON directly from evbuffer memory (no per-message copy).
        // We keep YYJSON_READ_INSITU disabled to avoid mutating buffer data.
        yyjson_read_err err;
        yyjson_doc* doc_raw = yyjson_read_opts(reinterpret_cast<char*>(data_ptr), msg_len,
                                               0,  // ✅ 不使用 INSITU
                                               nullptr, &err);

        if (!doc_raw) {
            // JSON parse error
            GW_LOG("[Gateway][ERROR] JSON parse error (len=%u): %s\n", msg_len, err.msg);
            GW_LOG("[Gateway][ERROR] Message content: %.*s\n",
                   (int)(msg_len > 200 ? 200 : msg_len),
                   reinterpret_cast<const char*>(data_ptr));
            evbuffer_drain(input, GATEWAY_HEADER_SIZE + msg_len);  // Drain the bad message
            continue;
        }

        // ✅ 使用 RAII 保护 JSON 文档，防止 longjmp 导致泄漏
        JsonDocGuard doc(doc_raw);

        // ⚠️ CRITICAL: Drain AFTER parsing, BEFORE dispatch
        // This ensures:
        // 1. Buffer state is correct if dispatch_message crashes
        // 2. We don't lose the message if parse fails
        evbuffer_drain(input, GATEWAY_HEADER_SIZE + msg_len);

        // Successfully parsed, dispatch message
        yyjson_val* msg = doc.root();
        dispatch_message(master_fd, msg);

        auto current = g_masters.find(master_fd);
        if (current == g_masters.end() || current->second->bev != bev) {
            return;
        }
        current->second->messages_received++;

        // ✅ doc 自动释放（即使 longjmp）

        batch_count++;  // 计数器递增
    }

    // libevent 会自动重新触发此回调，如果有更多数据可用
}

static void conn_eventcb(struct bufferevent *bev, short events, void *ctx) {
    GatewayMaster *master = (GatewayMaster *)ctx;

    if (events & BEV_EVENT_EOF) {
        remove_master(master->fd, "EOF");
    } else if (events & BEV_EVENT_ERROR) {
        remove_master(master->fd, "socket error");
    } else if (events & BEV_EVENT_TIMEOUT) {
        remove_master(master->fd, "timeout");
    }
}

static void listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
                        struct sockaddr *sa, int socklen, void *ctx) {
    char ip_str[INET6_ADDRSTRLEN] = {0};

    if (sa->sa_family == AF_INET) {
        evutil_inet_ntop(AF_INET, &(((struct sockaddr_in*)sa)->sin_addr), ip_str, sizeof(ip_str));
    } else if (sa->sa_family == AF_INET6) {
        evutil_inet_ntop(AF_INET6, &(((struct sockaddr_in6*)sa)->sin6_addr), ip_str, sizeof(ip_str));
    }

    GW_DEBUG("[Gateway] New connection from %s\n", ip_str);

    if ((int)g_masters.size() >= g_gateway_max_masters) {
        GW_LOG("[Gateway] Connection rejected: too many masters (%d)\n", (int)g_masters.size());
        evutil_closesocket(fd);
        return;
    }

    set_tcp_options(fd);

    struct event_base *base = evconnlistener_get_base(listener);
    // ⭐ 添加线程安全和延迟回调选项
    struct bufferevent *bev = bufferevent_socket_new(base, fd,
        BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE | BEV_OPT_DEFER_CALLBACKS);
    if (!bev) {
        evutil_closesocket(fd);
        return;
    }

    int id = generate_safe_id();
    if (id < 0) {
        bufferevent_free(bev);
        GW_LOG("[Gateway] Connection rejected: cannot generate ID\n");
        return;
    }

    auto master = std::make_unique<GatewayMaster>();
    master->fd = id;
    master->bev = bev;
    master->ip = ip_str;

    // ⭐ 设置读写水位线，避免高并发下内存消耗过大
    // 读水位：至少有 4 字节（一个长度头）才触发读回调
    bufferevent_setwatermark(bev, EV_READ, GATEWAY_HEADER_SIZE, 0);
    // 写水位：无限制（允许任意大小写入）
    bufferevent_setwatermark(bev, EV_WRITE, 0, 0);

    bufferevent_setcb(bev, conn_readcb, nullptr, conn_eventcb, master.get());
    bufferevent_enable(bev, EV_READ | EV_WRITE);

    g_masters[id] = std::move(master);
    g_stats.total_connections++;

    GW_DEBUG("[Gateway] Master connected: fd=%d, ip=%s (total: %zu)\n", id, ip_str, g_masters.size());
}

// ============================================================
// 发送函数
// ============================================================

static void evbuffer_owned_free_cb(const void *data, size_t, void *) {
    free(const_cast<void *>(data));
}

int gateway_send_raw_to_fd(int fd, const char *data, size_t len) {
    auto it = g_masters.find(fd);
    if (it == g_masters.end()) return 0;

    GatewayMaster *master = it->second.get();

    if (!master->bev || master->marked_for_delete) return 0;

    struct evbuffer *output = bufferevent_get_output(master->bev);
    size_t output_len = evbuffer_get_length(output);
    size_t incoming_len = GATEWAY_HEADER_SIZE + len;
    if (output_len + incoming_len > g_gateway_max_packet_size) {
        GW_LOG("[Gateway] Send buffer overflow: fd=%d, len=%zu, incoming=%zu\n",
               fd, output_len, incoming_len);
        remove_master(fd, "send buffer overflow");
        return 0;
    }

    // 构造完整消息 [长度头][数据]，快速写入。
    uint32_t len_net = htonl((uint32_t)len);
    if (evbuffer_add(output, &len_net, GATEWAY_HEADER_SIZE) != 0 ||
        evbuffer_add(output, data, len) != 0) {
        remove_master(fd, "send buffer write failed");
        return 0;
    }

    master->messages_sent++;
    g_stats.messages_sent++;
    g_stats.bytes_sent += len;

    return 1;
}

int gateway_send_raw_to_fd_owned(int fd, char *data, size_t len) {
  if (!data) return 0;

  auto it = g_masters.find(fd);
  if (it == g_masters.end()) {
    free(data);
    return 0;
  }

  GatewayMaster *master = it->second.get();
  if (!master->bev || master->marked_for_delete) {
    free(data);
    return 0;
  }

  struct evbuffer *output = bufferevent_get_output(master->bev);
  size_t output_len = evbuffer_get_length(output);
  size_t incoming_len = GATEWAY_HEADER_SIZE + len;
  if (output_len + incoming_len > g_gateway_max_packet_size) {
    GW_LOG("[Gateway] Send buffer overflow: fd=%d, len=%zu, incoming=%zu\n", fd, output_len,
           incoming_len);
    free(data);
    remove_master(fd, "send buffer overflow");
    return 0;
  }

  uint32_t len_net = htonl((uint32_t)len);
  if (evbuffer_add(output, &len_net, GATEWAY_HEADER_SIZE) != 0) {
    free(data);
    return 0;
  }
  if (evbuffer_add_reference(output, data, len, evbuffer_owned_free_cb, nullptr) != 0) {
    free(data);
    remove_master(fd, "send buffer reference failed");
    return 0;
  }

  master->messages_sent++;
  g_stats.messages_sent++;
  g_stats.bytes_sent += len;
  return 1;
}

int gateway_ping_master(int master_fd) {
    const char* ping_msg = "{\"type\":\"sys\",\"action\":\"ping\"}";
    size_t len = strlen(ping_msg);
    int result = gateway_send_raw_to_fd(master_fd, ping_msg, len);
    GW_DEBUG("[Gateway] Ping sent to fd=%d: %s\n", master_fd, result ? "OK" : "FAILED");
    return result;
}

void gateway_check_heartbeat_timeouts(void) {
    std::vector<int> to_remove;
    for (const auto& pair : g_masters) {
        GatewayMaster* master = pair.second.get();
        if (master->is_timeout()) {
            GW_LOG("[Gateway] Heartbeat timeout: fd=%d, ip=%s\n", master->fd, master->ip.c_str());
            to_remove.push_back(master->fd);
        }
    }
    for (int fd : to_remove) {
        remove_master(fd, "heartbeat timeout");
    }
}

// ============================================================
// 心跳定时器回调
// ============================================================

static void heartbeat_timer_cb(evutil_socket_t, short, void *) {
  gateway_check_heartbeat_timeouts();
  gateway_check_session_timeouts();
  if (g_heartbeat_timer && g_gateway_heartbeat_interval > 0) {
    struct timeval tv;
    tv.tv_sec = g_gateway_heartbeat_interval;
    tv.tv_usec = 0;
    evtimer_add(g_heartbeat_timer, &tv);
  }
}

static int start_heartbeat_timer() {
    if (g_heartbeat_timer) {
        evtimer_del(g_heartbeat_timer);
        g_heartbeat_timer = nullptr;
    }
    if (g_gateway_heartbeat_interval <= 0) return 0;

    g_heartbeat_timer = evtimer_new(g_event_base, heartbeat_timer_cb, nullptr);
    if (!g_heartbeat_timer) {
        GW_LOG("[Gateway] Failed to create heartbeat timer\n");
        return 0;
    }
    struct timeval tv;
    tv.tv_sec = g_gateway_heartbeat_interval;
    tv.tv_usec = 0;
    evtimer_add(g_heartbeat_timer, &tv);
    GW_LOG("[Gateway] Heartbeat timer started: interval=%d seconds\n", g_gateway_heartbeat_interval);
    return 1;
}

static void stop_heartbeat_timer() {
    if (g_heartbeat_timer) {
        evtimer_del(g_heartbeat_timer);
        g_heartbeat_timer = nullptr;
    }
}

// ============================================================
// Efun 实现
// ============================================================

void f_gateway_listen() {
    int num_args = st_num_arg;
    int port = (sp - num_args + 1)->u.number;
    int external = (num_args >= 2) ? (sp - num_args + 2)->u.number : 0;

    pop_n_elems(num_args);

    if (port <= 0) {
        push_number(0);
        return;
    }

    if (g_listener) {
        evconnlistener_free(g_listener);
        g_listener = nullptr;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = external ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(port);

    g_listener = evconnlistener_new_bind(
        g_event_base, listener_cb, nullptr,
        LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
        (struct sockaddr*)&sin, sizeof(sin));

    if (!g_listener) {
        push_number(0);
        return;
    }

    debug_message("Gateway listening on %s:%d\n", external ? "0.0.0.0" : "127.0.0.1", port);
    push_number(1);
}

void f_gateway_status() {
    mapping_t *map = allocate_mapping(10);
    svalue_t key, val;

    auto add_int = [&](const char* k, int v) {
        key.type = T_STRING; key.subtype = STRING_MALLOC;
        key.u.string = string_copy(k, "gw_status");
        val.type = T_NUMBER; val.u.number = v;
        svalue_t* dest = find_for_insert(map, &key, 0);
        if (dest) assign_svalue_no_free(dest, &val);
        free_svalue(&key, "gw_status");
    };

    auto add_str = [&](const char* k, const char* v) {
        key.type = T_STRING; key.subtype = STRING_MALLOC;
        key.u.string = string_copy(k, "gw_status");
        val.type = T_STRING; val.subtype = STRING_MALLOC;
        val.u.string = string_copy(v, "gw_status");
        svalue_t* dest = find_for_insert(map, &key, 0);
        if (dest) assign_svalue(dest, &val);
        free_svalue(&key, "gw_status");
        free_svalue(&val, "gw_status");
    };

    time_t uptime = time(nullptr) - g_stats.started_at;

    add_int("listening", g_listener ? 1 : 0);
    add_int("masters", (int)g_masters.size());
    add_int("sessions", gateway_get_session_count());
    add_int("debug", g_gateway_debug);
    add_int("max_packet_size", (int)g_gateway_max_packet_size);
    add_int("max_masters", g_gateway_max_masters);
    add_int("heartbeat_interval", g_gateway_heartbeat_interval);
    add_int("uptime", (int)uptime);
    add_str("heartbeat_timer", g_heartbeat_timer ? "active" : "inactive");

    push_refed_mapping(map);
}

void f_gateway_config() {
    int num_args = st_num_arg;
    const char* key = (sp - num_args + 1)->u.string;
    svalue_t* val = (num_args >= 2) ? sp : nullptr;

    if (!key) {
        pop_n_elems(num_args);
        push_number(0);
        return;
    }

    if (strcmp(key, "max_sessions") == 0) {
        if (val && val->type == T_NUMBER) g_gateway_max_sessions = val->u.number;
        pop_n_elems(num_args);
        push_number(g_gateway_max_sessions);
    } else if (strcmp(key, "timeout") == 0 || strcmp(key, "heartbeat_timeout") == 0) {
        if (val && val->type == T_NUMBER) g_gateway_heartbeat_timeout = val->u.number;
        pop_n_elems(num_args);
        push_number(g_gateway_heartbeat_timeout);
    } else if (strcmp(key, "debug") == 0) {
        if (val && val->type == T_NUMBER) g_gateway_debug = (val->u.number != 0);
        pop_n_elems(num_args);
        push_number(g_gateway_debug);
    } else if (strcmp(key, "max_masters") == 0) {
        if (val && val->type == T_NUMBER) g_gateway_max_masters = val->u.number;
        pop_n_elems(num_args);
        push_number(g_gateway_max_masters);
    } else if (strcmp(key, "heartbeat_interval") == 0) {
        if (val && val->type == T_NUMBER) {
            g_gateway_heartbeat_interval = val->u.number;
            start_heartbeat_timer();
        }
        pop_n_elems(num_args);
        push_number(g_gateway_heartbeat_interval);
    } else if (strcmp(key, "max_packet_size") == 0) {
        if (val && val->type == T_NUMBER) {
            auto requested = static_cast<size_t>(val->u.number);
            if (requested < 1024) {
                g_gateway_max_packet_size = 1024;
            } else if (requested > GATEWAY_DEFAULT_MAX_PACKET_SIZE) {
                g_gateway_max_packet_size = GATEWAY_DEFAULT_MAX_PACKET_SIZE;
            } else {
                g_gateway_max_packet_size = requested;
            }
        }
        pop_n_elems(num_args);
        push_number((int)g_gateway_max_packet_size);
    } else {
        pop_n_elems(num_args);
        push_number(0);
    }
}

void f_gateway_set_heartbeat() {
    int num_args = st_num_arg;
    int interval = (sp - num_args + 1)->u.number;
    int timeout = (num_args >= 2) ? (sp - num_args + 2)->u.number : g_gateway_heartbeat_timeout;

    pop_n_elems(num_args);

    if (interval > 0) g_gateway_heartbeat_interval = interval;
    if (timeout > 0) g_gateway_heartbeat_timeout = timeout;

    start_heartbeat_timer();
    push_number(1);
}

void f_gateway_check_timeout() {
    pop_stack();
    gateway_check_heartbeat_timeouts();
    gateway_check_session_timeouts();
    push_number(1);
}

void f_gateway_ping_master() {
    int master_fd = sp->u.number;
    pop_stack();
    int result = 0;

    if (master_fd <= 0) {
        auto master_ids = collect_master_ids();
        for (int fd : master_ids) {
            result += gateway_ping_master(fd);
        }
    } else {
        result = gateway_ping_master(master_fd);
    }
    push_number(result);
}

void f_is_gateway_user() {
    object_t* ob = sp->u.ob;
    pop_stack();
    int result = gateway_is_session(ob) ? 1 : 0;
    push_number(result);
}

/**
 * [FIXED] gateway_send
 * - 修正了 save_context 的使用方式
 * - 恢复 try-catch 结构来处理 C++ 异常
 */
void f_gateway_send() {
    int num_args = st_num_arg;
    svalue_t* data_sv = (sp - num_args + 1);

    if (data_sv->type != T_MAPPING) {
        GW_LOG("[Gateway] gateway_send: first arg must be mapping\n");
        pop_n_elems(num_args);
        push_number(0);
        return;
    }

    svalue_t* users_arg = (num_args >= 2) ? (sp - num_args + 2) : nullptr;
    bool has_user_filter = users_arg && !(users_arg->type == T_NUMBER && users_arg->u.number == 0);

    if (g_masters.empty()) {
        pop_n_elems(num_args);
        push_number(0);
        return;
    }

    // [FIX] 正确的 context 保存逻辑
    error_context_t econ;
    save_context(&econ);

    int send_result = 0;

    try {
        if (has_user_filter) {
            std::unordered_map<int, std::vector<std::string>> cids_by_master;

            auto collect = [&](object_t* ob) {
                if (!is_object_valid(ob) || !ob->interactive ||
                    !(ob->interactive->iflags & GATEWAY_SESSION)) {
                    return;
                }
                int master_fd = ob->interactive->gateway_master_fd;
                const char* sid = ob->interactive->gateway_session_id;
                if (!sid || !sid[0] || master_fd <= 0) return;

                auto mit = g_masters.find(master_fd);
                if (mit == g_masters.end()) return;
                if (!mit->second->bev || mit->second->marked_for_delete) return;

                cids_by_master[master_fd].emplace_back(sid);
            };

            if (users_arg->type == T_OBJECT) {
                collect(users_arg->u.ob);
            } else if (users_arg->type == T_ARRAY) {
                array_t* arr = users_arg->u.arr;
                for (int i = 0; i < arr->size; i++) {
                    if (arr->item[i].type == T_OBJECT) {
                        collect(arr->item[i].u.ob);
                    }
                }
            }

            // 指定了 users，但没有任何有效 gateway 目标：不应退化为广播。
            if (cids_by_master.empty()) {
                pop_context(&econ);
                pop_n_elems(num_args);
                push_number(0);
                return;
            }

            for (auto& pair : cids_by_master) {
                int master_fd = pair.first;
                auto& cids = pair.second;
                if (cids.empty()) continue;

                yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
                if (!doc) continue;
                CircularChecker checker;

                yyjson_mut_val* data_json = svalue_to_json_impl(doc, data_sv, &checker, 0);
                if (!data_json || !yyjson_mut_is_obj(data_json)) {
                    yyjson_mut_doc_free(doc);
                    continue;
                }

                yyjson_mut_val* cids_array = yyjson_mut_arr(doc);
                if (!cids_array) {
                    yyjson_mut_doc_free(doc);
                    continue;
                }
                for (const auto& sid : cids) {
                    yyjson_mut_arr_add_strncpy(doc, cids_array, sid.c_str(), sid.size());
                }
                yyjson_mut_obj_add_val(doc, data_json, "cids", cids_array);
                yyjson_mut_doc_set_root(doc, data_json);

                size_t final_len = 0;
                char* final_json = yyjson_mut_write(doc, 0, &final_len);
                yyjson_mut_doc_free(doc);
                if (!final_json) continue;

                if (gateway_send_raw_to_fd_owned(master_fd, final_json, final_len)) {
                    send_result = 1;
                }
            }
        } else {
            yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
            if (!doc) {
                pop_context(&econ);
                pop_n_elems(num_args);
                push_number(0);
                return;
            }
            CircularChecker checker;

            yyjson_mut_val* data_json = svalue_to_json_impl(doc, data_sv, &checker, 0);
            if (data_json) {
                yyjson_mut_doc_set_root(doc, data_json);
                size_t final_len = 0;
                char* final_json = yyjson_mut_write(doc, 0, &final_len);
                if (final_json) {
                    auto master_ids = collect_master_ids();
                    for (int master_fd : master_ids) {
                        auto it = g_masters.find(master_fd);
                        if (it == g_masters.end()) {
                            continue;
                        }
                        GatewayMaster* master = it->second.get();
                        if (!master->bev || master->marked_for_delete) continue;
                        if (gateway_send_raw_to_fd(master->fd, final_json, final_len)) {
                            send_result = 1;
                        }
                    }
                    free(final_json);
                }
            }
            yyjson_mut_doc_free(doc);
        }

    } catch (...) {
        // [FIX] 捕获异常后恢复上下文
        GW_LOG("[Gateway] Exception caught in gateway_send\n");
        restore_context(&econ);
        pop_context(&econ);
        
        pop_n_elems(num_args);
        push_number(0);
        return;
    }

    pop_context(&econ);
    pop_n_elems(num_args);
    push_number(send_result);
}

// ============================================================
// 初始化和清理
// ============================================================

int gateway_get_master_count() {
    return (int)g_masters.size();
}

void cleanup_gateway_masters() {
    stop_heartbeat_timer();
    g_masters.clear();
    if (g_listener) {
        evconnlistener_free(g_listener);
        g_listener = nullptr;
    }
}

void init_gateway() {
    g_stats.started_at = time(nullptr);

    int port = CONFIG_INT(__RC_GATEWAY_PORT__);
    int external = CONFIG_INT(__RC_GATEWAY_EXTERNAL__);
    g_gateway_debug = CONFIG_INT(__RC_GATEWAY_DEBUG__);

    int packet_size = CONFIG_INT(__RC_GATEWAY_PACKET_SIZE__);
    if (packet_size > 0) {
        g_gateway_max_packet_size = (size_t)packet_size;
    }

    if (port > 0) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = external ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
        sin.sin_port = htons(port);

        g_listener = evconnlistener_new_bind(
            g_event_base, listener_cb, nullptr,
            LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
            (struct sockaddr*)&sin, sizeof(sin));

        if (g_listener) {
            GW_LOG("Accepting [Gateway] connections on %s:%d\n", external ? "0.0.0.0" : "127.0.0.1", port);
            start_heartbeat_timer();
        } else {
            GW_LOG("Gateway: Failed to start listener on port %d\n", port);
        }
    }
}

void cleanup_gateway() {
    extern void cleanup_gateway_sessions();
    cleanup_gateway_sessions();
    cleanup_gateway_masters();
}
