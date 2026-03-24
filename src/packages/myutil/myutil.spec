// MyUtil Package - 实用工具集 v1.0
// 提供通用的工具函数

// ============================================================
// UUID 生成
// ============================================================
// 生成 UUID v4 格式字符串: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
// 用于: 唯一物品ID、交易ID、会话ID等
//
// 示例:
//   string item_id = uuid();        // "550e8400-e29b-41d4-a716-446655440000"
//   string tx_id = uuid();          // 交易唯一标识
//   string session_id = uuid();     // 会话唯一标识
string uuid();

// ============================================================
// StringBuffer (高性能字符串构建器)
// ============================================================
// 用于高效构建长字符串，避免 LPC 频繁拼接产生的临时对象开销
//
// 示例:
//   int buf = strbuf_new();
//   strbuf_add(buf, "Header\n");
//   strbuf_addf(buf, "Name: %s, Age: %d\n", name, age);
//   string result = strbuf_dump(buf); // 导出并销毁
//
// 注意: 如果创建后不调用 dump，必须手动管理生命周期（暂无自动GC）

int strbuf_new();
void strbuf_add(int , string );
void strbuf_addf(int , string , ...);
string strbuf_dump(int );
void strbuf_clear(int );
