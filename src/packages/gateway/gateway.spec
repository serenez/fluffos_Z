// Gateway Package - Efun 声明 v2.2
//
// 核心功能：生产环境必需

// ============================================================
// [核心] 网关监听
// ============================================================
int gateway_listen(int, int default:0);

// ============================================================
// [核心] 网关状态
// ============================================================
// 返回mapping: listening, masters, sessions, debug, max_packet_size,
//             max_masters, max_sessions, uptime, heartbeat_timer
mapping gateway_status();

// ============================================================
// [核心] 运行时配置
// ============================================================
// 支持的key:
//   "max_sessions" - 最大会话数
//   "max_masters" - 最大主连接数
//   "timeout" / "heartbeat_timeout" - 心跳超时(秒)
//   "heartbeat_interval" - 心跳间隔(秒)
//   "debug" - 调试开关
//   "max_packet_size" - 最大包大小
mixed gateway_config(string, mixed default:0);

// ============================================================
// [发送] 发送消息 (支持 RPC 回调)
// ============================================================
// 发送系统消息到网关（直接发送 data 的 JSON，不包装成 output 消息）
// callback: 可选的 RPC 回调函数，格式为 function(mapping response)
//          有回调时会自动生成 msg_id 并注入到 JSON 中
//          response 包含完整响应消息，超时时为 (["status": "timeout", "msg_id": "..."])
int gateway_send(mixed, mixed default:0);

// ============================================================
// [心跳] 心跳管理
// ============================================================
void gateway_set_heartbeat(int, int default:0);
void gateway_check_timeout();
int gateway_ping_master(int default:0);

// ============================================================
// [会话] 会话管理 (支持 RPC 回调)
// ============================================================
object *gateway_sessions();
mapping gateway_session_info(object);
// 给网关用户发送消息，支持 RPC 回调
// callback: 可选的 RPC 回调函数，格式为 function(mapping response)
//          有回调时会自动生成 msg_id 并注入到 JSON 中
//          response 包含完整响应消息，超时时为 (["status": "timeout", "msg_id": "..."])
int gateway_session_send(object, mixed, mixed default:0);
int gateway_destroy_session(string);

// ============================================================
// [工具] 检查用户类型
// ============================================================
int is_gateway_user(object);

