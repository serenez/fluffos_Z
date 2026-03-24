/*
 * Gateway Session Manager - 使用 interactive_t 虚拟对象 (v2.5)
 * 创建虚拟 interactive 结构，让 gateway 用户走 FluffOS 原生消息通道
 *
 * v2.5 修复 (2026-01-05):
 * - S-4: 简化消息包装逻辑
 *   - Mapping 直接加 cid，不再检查是否有 type 字段
 *   - 非 Mapping 统一包装成 {"type": "output", "cid": "...", "data": 原值}
 *   - JSON 序列化失败时返回 0
 *
 * v2.4 修复 (2026-01-05):
 * - S-3: 新增 UID 校验（user_ob_load_time）防止 ABA 问题
 * - 零拷贝优化：gateway_inject_input_internal 避免 std::string 分配
 * - 增强边界检查和错误日志
 *
 * v2.3 修复：
 * - S-2: 增强会话映射安全性，添加对象有效性验证
 * - 修复野指针访问风险
 */

#include "base/std.h"
#include "gateway.h"

#include "vm/vm.h"
#include "vm/internal/base/machine.h"
#include "base/internal/stralloc.h"
#include "base/internal/external_port.h"
#include "comm.h"
#include "user.h"
#include "interactive.h"

#include <event2/event.h>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <cstring>
#include <ctime>
#include <chrono>
#include <vector>
#include <random>

// ============================================================
// 循环引用检测器（与 pkg_json.cc 中的实现保持一致）
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
// 外部函数声明
// ============================================================
extern int svalue_to_json_string(svalue_t*, char**, size_t*);
extern void json_to_svalue(yyjson_val*, svalue_t*, int);

// 导入 svalue_to_json_impl 用于性能优化
extern yyjson_mut_val* svalue_to_json_impl(yyjson_mut_doc* doc, svalue_t* sv,
                                             CircularChecker* checker, int depth);

// ============================================================
// 会话结构
// ============================================================

struct GatewaySession {
    std::string session_id;
    std::string real_ip;
    int real_port;
    int master_fd;
    time_t connected_at;
    time_t last_active;
    object_t* user_ob;
    int64_t user_ob_load_time; // UID 校验：防止 ABA 问题

    GatewaySession() : real_port(0), master_fd(-1),
                       connected_at(0), last_active(0), user_ob(nullptr),
                       user_ob_load_time(0) {}

    // 检查会话是否超时
    bool is_timeout(int timeout_sec) const {
        if (timeout_sec <= 0) return false;
        time_t now = time(nullptr);
        return (now - last_active) > timeout_sec;
    }
};

// 使用全局配置
extern int g_gateway_max_sessions;
extern int g_gateway_heartbeat_timeout;  // 复用作为会话超时

// 会话存储
static std::unordered_map<std::string, std::unique_ptr<GatewaySession>> g_sessions;
static std::unordered_map<object_t*, GatewaySession*> g_obj_to_session;

static void cleanup_temp_gateway_interactive(object_t* owner) {
    if (!owner || !owner->interactive) return;
    interactive_t* ip = owner->interactive;

    if (ip->ev_command) {
        evtimer_del(ip->ev_command);
        event_free(ip->ev_command);
        ip->ev_command = nullptr;
    }
    if (ip->gateway_session_id) {
        FREE_MSTR(ip->gateway_session_id);
        ip->gateway_session_id = nullptr;
    }
    if (ip->gateway_real_ip) {
        FREE_MSTR(ip->gateway_real_ip);
        ip->gateway_real_ip = nullptr;
    }

    user_del(ip);
    FREE(ip);
    owner->interactive = nullptr;
}

// ============================================================
// 对象有效性验证辅助函数 (v2.3)
// ============================================================

/**
 * 检查对象是否仍然有效且未销毁
 * 用于防止野指针访问
 */
static inline bool is_object_valid(object_t* ob) {
    if (!ob) return false;
    // 检查对象是否已销毁
    if (ob->flags & O_DESTRUCTED) return false;
    // 检查对象是否仍在活跃对象列表中
    // (通过检查对象名称是否非空来判断，因为销毁后 obname 会被清空)
    if (!ob->obname || ob->obname[0] == '\0') return false;
    return true;
}

// ============================================================
// Gateway 命令回调
// ============================================================

static void gateway_command_callback(evutil_socket_t, short, void* arg) {
    auto* user = reinterpret_cast<interactive_t*>(arg);
    if (!user) return;

    set_eval(max_eval_cost);
    process_user_command(user);
    current_interactive = nullptr;
}

// ============================================================
// 内部辅助函数
// ============================================================

static GatewaySession* find_session_by_id(const char* session_id) {
    if (!session_id) return nullptr;
    auto it = g_sessions.find(session_id);
    return (it != g_sessions.end()) ? it->second.get() : nullptr;
}

/**
 * 通过对象查找会话 (v2.4 - UID 校验防止 ABA 问题)
 * 使用 load_time 进行唯一性校验，防止地址重用导致的消息串路
 */
static GatewaySession* find_session_by_obj(object_t* ob) {
    if (!ob || !is_object_valid(ob)) return nullptr;

    auto it = g_obj_to_session.find(ob);
    if (it == g_obj_to_session.end()) return nullptr;

    GatewaySession* sess = it->second;
    if (!sess || !sess->user_ob) {
        // Session 无效，清理映射
        g_obj_to_session.erase(it);
        return nullptr;
    }

    // UID 校验：防止 ABA 问题
    // 即使地址相同，load_time 不同也说明是新对象
    if (!is_object_valid(sess->user_ob) ||
        sess->user_ob != ob ||
        sess->user_ob->load_time != sess->user_ob_load_time) {
        // 对象已销毁、不匹配或已被新对象替换（ABA），清理映射
        GW_LOG("[Gateway] Object mismatch or reused (ABA detected): ob=%p, expected_load_time=%lld, actual=%lld\n",
               ob, sess->user_ob_load_time, ob->load_time);
        g_obj_to_session.erase(it);
        return nullptr;
    }

    return sess;
}

// ============================================================
// 外部声明
// ============================================================

extern struct event_base* g_event_base;

// ============================================================
// 内部接口实现 - 供 comm.cc 调用
// ============================================================


/**
 * 发送数据到指定 session (内部接口)
 * 优化：使用 build_gateway_output_packet 避免手动拼接和 svalue 转换
 */
int gateway_send_to_session(const char* session_id, const char* data, size_t len) {
    // 增强安全检查：验证 session_id 非空且长度合理
    if (!session_id || strlen(session_id) == 0 || strlen(session_id) > 256) {
        GW_DEBUG("[Gateway] gateway_send_to_session: invalid session_id\n");
        return 0;
    }

    GatewaySession* sess = find_session_by_id(session_id);
    if (!sess || sess->master_fd < 0) return 0;

    char* final_json = nullptr;
    size_t final_len = 0;

    // 使用 yyjson 帮手构建: {"type":"output","cid":"...","data":"..."}
    if (!build_gateway_output_packet(sess->session_id.c_str(), data, &final_json, &final_len)) {
        return 0;
    }

    // 发送
    if (final_json) {
        gateway_send_raw_to_fd_owned(sess->master_fd, final_json, final_len);
    }

    return 1;
}

bool gateway_is_session(object_t* ob) {
    if (!ob || !ob->interactive) return false;
    return (ob->interactive->iflags & GATEWAY_SESSION) != 0;
}

const char* gateway_get_ip(object_t* ob) {
    if (!ob || !ob->interactive || !(ob->interactive->iflags & GATEWAY_SESSION)) {
        return nullptr;
    }
    GatewaySession* sess = find_session_by_obj(ob);
    return sess ? sess->real_ip.c_str() : nullptr;
}

// ============================================================
// 会话创建
// ============================================================

/**
 * 创建会话（内部接口）
 * data_val: Go 发送的完整 login data，会传递给 master->gateway_connect(data)
 */
object_t* gateway_create_session_internal(int master_fd, const char* session_id,
                                           svalue_t* data_val,
                                           const char* ip, int port) {
    GW_DEBUG("[Gateway] gateway_create_session_internal: session_id='%s' (len=%zu)\n",
           session_id ? session_id : "(null)", session_id ? strlen(session_id) : 0);

    if (master_fd <= 0 || !session_id || strlen(session_id) == 0) {
        GW_LOG("[Gateway] Invalid parameters: master_fd=%d, session_id=%p\n",
               master_fd, session_id);
        return nullptr;
    }

    // 检查会话是否已存在
    if (g_sessions.count(session_id)) {
        GW_LOG("[Gateway] Session already exists: %s\n", session_id);
        return nullptr;
    }

    // 检查会话数量限制
    if ((int)g_sessions.size() >= g_gateway_max_sessions) {
        GW_LOG("[Gateway] Max sessions reached (%d)\n", g_gateway_max_sessions);
        return nullptr;
    }

    // 创建会话
    auto sess = std::make_unique<GatewaySession>();
    sess->session_id = session_id;
    sess->master_fd = master_fd;
    sess->connected_at = time(nullptr);
    sess->last_active = sess->connected_at;
    sess->real_ip = ip ? ip : "";
    sess->real_port = port;

    GatewaySession* sess_ptr = sess.get();
    g_sessions[session_id] = std::move(sess);

    // 调用 master->connect() 创建登录对象（复用现有逻辑）
    save_command_giver(master_ob);
    master_ob->flags |= O_ONCE_INTERACTIVE;

    // 创建虚拟 interactive_t
    interactive_t* user = user_add();
    user->connection_type = PORT_TYPE_GATEWAY;
    user->ob = master_ob;
    user->last_time = get_current_time();
    user->fd = -1;  // 虚拟会话没有真正的 fd
    user->local_port = 0;
    user->iflags |= GATEWAY_SESSION;

    // 设置 gateway 相关字段
    user->gateway_session_id = string_copy(session_id, "gateway_session_id");
    user->gateway_real_ip = string_copy(sess_ptr->real_ip.c_str(), "gateway_real_ip");
    user->gateway_real_port = sess_ptr->real_port;
    user->gateway_master_fd = master_fd;

    // 创建命令处理定时器
    user->ev_command = evtimer_new(g_event_base, gateway_command_callback, user);
    if (!user->ev_command) {
        GW_LOG("[Gateway] Failed to create command event for session %s\n", session_id);
        if (user->gateway_session_id) FREE_MSTR(user->gateway_session_id);
        if (user->gateway_real_ip) FREE_MSTR(user->gateway_real_ip);
        user_del(user);
        FREE(user);
        master_ob->flags &= ~O_ONCE_INTERACTIVE;
        g_sessions.erase(session_id);
        return nullptr;
    }

    master_ob->interactive = user;

    // 调用 master->connect() (无参数，复用现有登录逻辑)
    set_eval(max_eval_cost);
    svalue_t* ret = safe_apply_master_ob(APPLY_CONNECT, 0);
    restore_command_giver();

    if (ret == nullptr || ret == (svalue_t*)-1 || ret->type != T_OBJECT) {
        GW_LOG("[Gateway] connect() failed for session %s: ret=%p, type=%d\n",
              session_id, ret, ret ? ret->type : -1);
        cleanup_temp_gateway_interactive(master_ob);
        master_ob->flags &= ~O_ONCE_INTERACTIVE;
        g_sessions.erase(session_id);
        return nullptr;
    }

    // 将 interactive 转移到返回的对象
    object_t* ob = ret->u.ob;
    ob->interactive = master_ob->interactive;
    ob->interactive->ob = ob;
    ob->flags |= O_ONCE_INTERACTIVE;
    ob->interactive->iflags |= (HAS_WRITE_PROMPT | HAS_PROCESS_INPUT);

    master_ob->flags &= ~O_ONCE_INTERACTIVE;
    master_ob->interactive = nullptr;

    add_ref(ob, "gateway_create_session");

    // 保存会话关联（包含 load_time 用于 UID 校验）
    sess_ptr->user_ob = ob;
    sess_ptr->user_ob_load_time = ob->load_time; // 保存创建时间作为 UID
    g_obj_to_session[ob] = sess_ptr;

    GW_DEBUG("[Gateway] Session created: '%s' -> %s\n", sess_ptr->session_id.c_str(), ob->obname);

    // 调用 gateway_logon(data) 替代原来的 logon()
    // 使用 safe_apply 保证 LPC 运行时错误不会导致驱动崩溃
    // 行为与普通连接上的 safe_apply(APPLY_LOGON, ...) 保持一致
    save_command_giver(ob);
    current_interactive = ob;
    push_svalue(data_val);
    set_eval(max_eval_cost);
    svalue_t* logon_ret = safe_apply("gateway_logon", ob, 1, ORIGIN_DRIVER);
    restore_command_giver();
    current_interactive = nullptr;

    if (logon_ret == nullptr) {
        GW_LOG("[Gateway] gateway_logon() failed for session %s, disconnecting user\n", session_id);
        if (ob->interactive) {
            remove_interactive(ob, 0);
        }
        return nullptr;
    }

    if (ob->flags & O_DESTRUCTED) {
        if (ob->interactive) {
            remove_interactive(ob, 1);
        }
        return nullptr;
    }

    return ob;
}

// ============================================================
// 会话销毁
// ============================================================

int gateway_destroy_session_internal(const char* session_id, const char* reason_code,
                                     const char* reason_text) {
    if (!session_id) return 0;

    auto it = g_sessions.find(session_id);
    if (it == g_sessions.end()) return 0;

    GatewaySession* sess = it->second.get();
    object_t* ob = sess->user_ob;

    if (ob && ob->interactive) {
        // 显式通知 LPC 对象 gateway_disconnected(reason_code, reason_text)。
        // remove_interactive() 随后仍会触发 net_dead()，保持兼容旧流程。
        const char* reason_code_str =
            (reason_code && reason_code[0] != '\0') ? reason_code : "client_disconnected";
        const char* reason_text_str =
            (reason_text && reason_text[0] != '\0') ? reason_text : reason_code_str;
        save_command_giver(ob);
        set_eval(max_eval_cost);
        push_malloced_string(string_copy(reason_code_str, "gateway_disconnected_reason_code"));
        push_malloced_string(string_copy(reason_text_str, "gateway_disconnected_reason_text"));
        safe_apply("gateway_disconnected", ob, 2, ORIGIN_DRIVER);
        restore_command_giver();

        sess->master_fd = -1;  // 避免发送回环通知
        remove_interactive(ob, 0);
    } else {
        if (ob) g_obj_to_session.erase(ob);
        g_sessions.erase(it);
    }

    GW_DEBUG("[Gateway] Session destroyed: %s (%s:%s)\n", session_id,
             reason_code ? reason_code : "", reason_text ? reason_text : "");
    return 1;
}

// ============================================================
// 查找用户 (v2.3 - 增强安全性)
// ============================================================

object_t* gateway_find_user_by_session(const char* session_id) {
    GatewaySession* sess = find_session_by_id(session_id);
    if (!sess || !sess->user_ob) {
        return nullptr;
    }

    // 使用增强的验证函数
    if (!is_object_valid(sess->user_ob)) {
        GW_DEBUG("[Gateway] User object invalid for session %s\n", session_id);
        return nullptr;
    }

    return sess->user_ob;
}

// ============================================================
// 注入输入
// ============================================================

int gateway_inject_input_internal(object_t* user, const char* input) {
    if (!user || !user->interactive || !(user->interactive->iflags & GATEWAY_SESSION)) {
        return 0;
    }

    interactive_t* ip = user->interactive;

    // 原地计算修剪后的长度，避免 std::string 分配（零拷贝优化）
    size_t input_len = strlen(input);

    // 从尾部移除换行符
    while (input_len > 0 &&
           (input[input_len - 1] == '\n' || input[input_len - 1] == '\r')) {
        input_len--;
    }

    if (input_len == 0) return 0;

    // 边界检查：确保缓冲区空间足够
    size_t available = sizeof(ip->text) - ip->text_end - 2; // -2 for '\n' and '\0'
    if (input_len > available) {
        GW_DEBUG("[Gateway] inject_input: buffer full (need=%zu, avail=%zu)\n",
                 input_len, available);
        return 0;
    }

    // 直接 memcpy，避免中间字符串分配
    memcpy(ip->text + ip->text_end, input, input_len);
    ip->text_end += input_len;
    ip->text[ip->text_end++] = '\n';
    ip->text[ip->text_end] = '\0';

    // 更新活跃时间
    GatewaySession* sess = find_session_by_obj(user);
    if (sess) sess->last_active = time(nullptr);

    // 调度命令处理
    if (cmd_in_buf(ip)) {
        ip->iflags |= CMD_IN_BUF;
        if (ip->ev_command) {
            struct timeval zero = {0, 0};
            evtimer_del(ip->ev_command);
            evtimer_add(ip->ev_command, &zero);
        }
    }

    return 1;
}

// ============================================================
// exec() 映射更新 (v2.3 - 增强安全性)
// ============================================================

void gateway_session_exec_update(object_t* new_ob, object_t* old_ob) {
    if (!old_ob || !new_ob) return;

    // 验证新对象有效性
    if (!is_object_valid(new_ob)) {
        GW_DEBUG("[Gateway] exec: new object invalid\n");
        return;
    }

    auto it = g_obj_to_session.find(old_ob);
    if (it == g_obj_to_session.end()) return;

    GatewaySession* sess = it->second;
    if (!sess) return;

    GW_DEBUG("[Gateway] exec: %s -> %s\n",
             old_ob->obname ? old_ob->obname : "null",
             new_ob->obname ? new_ob->obname : "null");

    auto node = g_obj_to_session.extract(old_ob);
    if (node) {
        node.key() = new_ob;
        g_obj_to_session.insert(std::move(node));
        sess->user_ob = new_ob;
    }
}

// ============================================================
// 用户断开连接清理
// ============================================================

void gateway_handle_remove_interactive(interactive_t* ip) {
    if (!ip || !(ip->iflags & GATEWAY_SESSION) || !ip->ob) return;

    auto it = g_obj_to_session.find(ip->ob);
    if (it == g_obj_to_session.end()) return;

    GatewaySession* sess = it->second;
    std::string sid = sess->session_id;

    GW_DEBUG("[Gateway] User %s disconnected, cleaning session %s\n",
             ip->ob->obname, sid.c_str());

    g_obj_to_session.erase(ip->ob);
    g_sessions.erase(sid);
}

// ============================================================
// 清理指定主连接的所有会话
// ============================================================

void gateway_cleanup_master_sessions(int master_fd) {
    std::vector<std::string> to_remove;
    for (auto& pair : g_sessions) {
        if (pair.second->master_fd == master_fd) {
            to_remove.push_back(pair.first);
        }
    }

    for (const auto& id : to_remove) {
        gateway_destroy_session_internal(id.c_str(), "gateway_lost", "gateway lost");
    }

    GW_DEBUG("[Gateway] Cleaned %zu sessions for master fd %d\n",
             to_remove.size(), master_fd);
}

// ============================================================
// 状态查询
// ============================================================

int gateway_get_session_count() {
    return (int)g_sessions.size();
}

// ============================================================
// Efun 实现
// ============================================================

void f_gateway_destroy_session() {
    const char* session_id = sp->u.string;
    pop_stack();

    if (!session_id) {
        push_number(0);
        return;
    }

    int result = gateway_destroy_session_internal(session_id, "efun_destroy", "efun");
    push_number(result);
}

void f_gateway_sessions() {
    std::vector<object_t*> users;
    for (auto& pair : g_sessions) {
        object_t* ob = pair.second->user_ob;
        // 使用增强的验证函数
        if (ob && is_object_valid(ob)) {
            users.push_back(ob);
        }
    }

    array_t* arr = allocate_array(users.size());
    for (size_t i = 0; i < users.size(); i++) {
        arr->item[i].type = T_OBJECT;
        arr->item[i].u.ob = users[i];
        add_ref(users[i], "gateway_sessions");
    }

    push_refed_array(arr);
}

void f_gateway_session_info() {
    object_t* ob = sp->u.ob;
    pop_stack();

    // 使用增强的验证函数
    if (!ob || !is_object_valid(ob) || !ob->interactive ||
        !(ob->interactive->iflags & GATEWAY_SESSION)) {
        push_number(0);
        return;
    }

    GatewaySession* sess = find_session_by_obj(ob);
    if (!sess) {
        push_number(0);
        return;
    }

    mapping_t* map = allocate_mapping(6);
    svalue_t key, val;

    auto add_str = [&](const char* k, const char* v) {
        key.type = T_STRING; key.subtype = STRING_MALLOC;
        key.u.string = string_copy(k, "gw_info");
        val.type = T_STRING; val.subtype = STRING_MALLOC;
        val.u.string = string_copy(v, "gw_info");
        svalue_t* dest = find_for_insert(map, &key, 0);
        if (dest) assign_svalue(dest, &val);
        free_svalue(&key, "gw"); free_svalue(&val, "gw");
    };

    auto add_int = [&](const char* k, LPC_INT v) {
        key.type = T_STRING; key.subtype = STRING_MALLOC;
        key.u.string = string_copy(k, "gw_info");
        val.type = T_NUMBER; val.u.number = v;
        svalue_t* dest = find_for_insert(map, &key, 0);
        if (dest) assign_svalue_no_free(dest, &val);
        free_svalue(&key, "gw");
    };

    add_str("session_id", sess->session_id.c_str());
    add_str("ip", sess->real_ip.c_str());
    add_int("port", sess->real_port);
    add_int("connected_at", (LPC_INT)sess->connected_at);
    add_int("last_active", (LPC_INT)sess->last_active);

    push_refed_mapping(map);
}
/**
 * gateway_session_send(object ob, mixed data)
 * 向指定会话发送消息
 *
 * 优化版本：直接使用 svalue_to_json_impl，避免 JSON 双重序列化
 */
void f_gateway_session_send() {
    int num_args = st_num_arg;

    // 参数获取
    object_t* ob = (sp - num_args + 1)->u.ob;
    svalue_t* data = (sp - num_args + 2);

    // 1. 安全验证
    if (!ob) {
        GW_DEBUG("[Gateway] gateway_session_send: ob is NULL\n");
        pop_n_elems(num_args);
        push_number(0);
        return;
    }

    if (!is_object_valid(ob)) {
        GW_DEBUG("[Gateway] gateway_session_send: ob is not valid\n");
        pop_n_elems(num_args);
        push_number(0);
        return;
    }

    if (!ob->interactive) {
        GW_DEBUG("[Gateway] gateway_session_send: ob->interactive is NULL\n");
        pop_n_elems(num_args);
        push_number(0);
        return;
    }

    if (!(ob->interactive->iflags & GATEWAY_SESSION)) {
        GW_DEBUG("[Gateway] gateway_session_send: ob is not a GATEWAY_SESSION\n");
        pop_n_elems(num_args);
        push_number(0);
        return;
    }

    GatewaySession* sess = find_session_by_obj(ob);
    if (!sess) {
        GW_DEBUG("[Gateway] gateway_session_send: session not found for object\n");
        pop_n_elems(num_args);
        push_number(0);
        return;
    }

    if (sess->master_fd < 0) {
        GW_DEBUG("[Gateway] gateway_session_send: master_fd < 0\n");
        pop_n_elems(num_args);
        push_number(0);
        return;
    }

    // 2. 增加引用计数，防止对象在函数执行期间被销毁
    add_ref(ob, "gateway_session_send");

    sess->last_active = time(nullptr);

    // 3. 设置错误捕获上下文，防止 LPC 代码错误导致驱动崩溃
    error_context_t econ;
    save_context(&econ);

    // 4. 简化包装逻辑：不过度包装，保持 LPC 消息结构
    yyjson_mut_doc* doc = nullptr;
    int send_result = 0;

    try {
        doc = yyjson_mut_doc_new(nullptr);
        if (!doc) {
            debug_message("[Gateway] Failed to create JSON document\n");
            send_result = 0;
        } else {
            extern yyjson_mut_val* svalue_to_json_impl(yyjson_mut_doc* doc, svalue_t* sv,
                                                         CircularChecker* checker, int depth);
            CircularChecker checker;

            // 简化逻辑（v2.5）：
            // 1. Mapping → 直接加 cid 发送（LPC 层自己决定是否有 type）
            // 2. 非 Mapping → 统一包装成 {"type": "output", "cid": "...", "data": 原值}
            if (data->type == T_MAPPING) {
                // Mapping：直接加 cid，不检查是否有 type 字段
                yyjson_mut_val* data_val = svalue_to_json_impl(doc, data, &checker, 0);

                if (!data_val) {
                    yyjson_mut_doc_free(doc);
                    debug_message("[Gateway] Failed to serialize mapping data\n");
                    send_result = 0;
                } else {
                    // 添加 cid
                    yyjson_mut_obj_add_strncpy(doc, data_val, "cid",
                                              sess->session_id.c_str(),
                                              sess->session_id.length());
                    yyjson_mut_doc_set_root(doc, data_val);

                    GW_DEBUG("[Gateway] Sending mapping with cid\n");

                    // 序列化为最终 JSON
                    size_t packet_len = 0;
                    char* packet_json = yyjson_mut_write(doc, 0, &packet_len);

                    if (!packet_json) {
                        debug_message("[Gateway] Failed to write JSON\n");
                        send_result = 0;
                    } else {
                        GW_DEBUG("[Gateway] Sending to FluffOS: %s\n", packet_json);

                        // 发送
                        send_result = gateway_send_raw_to_fd_owned(sess->master_fd, packet_json, packet_len);
                    }
                }

            } else {
                // 非 Mapping：统一包装成 {"type": "output", "cid": "...", "data": 原值}
                yyjson_mut_val* root = yyjson_mut_obj(doc);
                yyjson_mut_doc_set_root(doc, root);

                yyjson_mut_obj_add_str(doc, root, "type", "output");
                yyjson_mut_obj_add_strncpy(doc, root, "cid",
                                          sess->session_id.c_str(),
                                          sess->session_id.length());

                yyjson_mut_val* data_val = svalue_to_json_impl(doc, data, &checker, 0);

                if (!data_val) {
                    yyjson_mut_doc_free(doc);
                    debug_message("[Gateway] Failed to serialize non-mapping data\n");
                    send_result = 0;
                } else {
                    yyjson_mut_obj_add_val(doc, root, "data", data_val);

                    GW_DEBUG("[Gateway] Sending non-mapping as output: type=%d\n", data->type);

                    // 序列化为最终 JSON
                    size_t packet_len = 0;
                    char* packet_json = yyjson_mut_write(doc, 0, &packet_len);

                    if (!packet_json) {
                        debug_message("[Gateway] Failed to write JSON\n");
                        send_result = 0;
                    } else {
                        GW_DEBUG("[Gateway] Sending to FluffOS: %s\n", packet_json);

                        // 发送
                        send_result = gateway_send_raw_to_fd_owned(sess->master_fd, packet_json, packet_len);
                    }
                }
            }

            // 清理 JSON 文档
            if (doc) {
                yyjson_mut_doc_free(doc);
            }
        }

    } catch (const char* err) {
        // 捕获 LPC 代码执行错误（如访问未初始化的 class 指针）
        debug_message("[Gateway] Error in gateway_session_send: %s\n", err);
        send_result = 0;

        // 清理 JSON 文档（如果已创建）
        if (doc) {
            yyjson_mut_doc_free(doc);
        }
    }

    // 5. 恢复错误上下文
    restore_context(&econ);
    pop_context(&econ);

    // 6. 释放引用计数
    free_object(&ob, "gateway_session_send");

    pop_n_elems(num_args);
    push_number(send_result);
}
// ============================================================
// 会话超时检查
// ============================================================

/**
 * 检查并清理超时会话
 */
void gateway_check_session_timeouts() {
    std::vector<std::string> to_remove;

    for (const auto& pair : g_sessions) {
        if (pair.second->is_timeout(g_gateway_heartbeat_timeout)) {
            to_remove.push_back(pair.first);
        }
    }

    for (const auto& sid : to_remove) {
        GW_DEBUG("[Gateway] Session timeout: %s\n", sid.c_str());
        gateway_destroy_session_internal(sid.c_str(), "session_timeout", "timeout");
    }
}

// ============================================================
// 清理 (v2.3 - 增强安全性)
// ============================================================

void cleanup_gateway_sessions() {
    std::vector<std::string> session_ids;
    session_ids.reserve(g_sessions.size());
    for (const auto& pair : g_sessions) {
        session_ids.push_back(pair.first);
    }

    for (const auto& session_id : session_ids) {
        auto it = g_sessions.find(session_id);
        if (it == g_sessions.end()) {
            continue;
        }

        object_t* ob = it->second->user_ob;
        if (ob && is_object_valid(ob) && ob->interactive) {
            remove_interactive(ob, 0);
        } else {
            if (ob) {
                g_obj_to_session.erase(ob);
            }
            g_sessions.erase(it);
        }
    }
    g_sessions.clear();
    g_obj_to_session.clear();
}
