/*
 * MyUtil Package - 实用工具集 v1.0
 * 提供通用的工具函数（UUID 等）
 */

#ifndef PACKAGES_MYUTIL_H
#define PACKAGES_MYUTIL_H

#include <string>

// ============================================================
// Efun 声明
// ============================================================

// UUID 生成
void f_uuid(void);

// ============================================================
// StringBuffer (高性能字符串构建器)
// ============================================================

void f_strbuf_new(void);
void f_strbuf_add(void);
void f_strbuf_addf(void);
void f_strbuf_dump(void);
void f_strbuf_clear(void);

// ============================================================
// C++ 辅函数 (供其他包调用)
// ============================================================

/**
 * 生成 UUID v4 字符串
 * 返回标准 UUID 格式: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 */
std::string generate_uuid_v4();

// ============================================================
// 初始化
// ============================================================

void init_myutil(void);
void cleanup_myutil(void);

#endif // PACKAGES_MYUTIL_H
