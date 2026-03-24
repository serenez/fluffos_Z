// ============================================================
// Xoroshiro128++ 种子系统包 (独立包版本)
// Final v3.0 - 极简直观版
// ============================================================

#include "base/package_api.h"

// ==================== 辅助函数 ====================

/**
 * 左旋转操作
 */
static inline uint64_t rotl(const uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

/**
 * Xoroshiro128++ 核心算法
 * 状态: 2 个 uint64_t (16 字节)
 * 周期: 2^128 - 1
 */
static inline uint64_t xoroshiro128pp(uint64_t s[2]) {
    const uint64_t result = rotl(s[0] + s[1], 17) * 5;

    s[1] ^= s[0];
    s[0] = rotl(s[0], 49) ^ s[1] ^ (s[1] << 21);
    s[1] = rotl(s[1], 28);

    return result;
}

/**
 * FNV-1a 64位哈希算法
 * 比 SplitMix64 快 10 倍，质量足够好
 */
static inline uint64_t fnv1a_hash(const char *str, int len) {
    uint64_t hash = 14695981039346656037ULL;
    for (int i = 0; i < len; i++) {
        hash ^= (uint8_t)str[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/**
 * 从 int/string 初始化状态
 * 返回: 0 = 成功, -1 = 负数种子, -2 = 空字符串, -3 = 类型错误
 */
static int init_state_from_input(svalue_t *input, uint64_t state[2]) {
    if (input->type == T_NUMBER) {
        int64_t seed_val = input->u.number;
        if (seed_val < 0) {
            return -1;
        }
        state[0] = (uint64_t)seed_val;
        state[1] = 0x9E3779B97F4A7C15ULL;
    } else if (input->type == T_STRING) {
        const char *str = input->u.string;
        int len = SVALUE_STRLEN(input);

        if (len == 0) {
            return -2;
        }

        uint64_t hash = fnv1a_hash(str, len);
        state[0] = hash;
        state[1] = fnv1a_hash(str, len) ^ 0x9E3779B97F4A7C15ULL;
    } else {
        return -3;
    }

    xoroshiro128pp(state);
    return 0;
}

/**
 * 从 buffer 加载状态
 * 返回: 0 = 成功, -1 = buffer 太小
 */
static inline int load_state_from_buffer(buffer_t *buf, uint64_t state[2]) {
    if (buf->size < 16) {
        return -1;
    }
    memcpy(state, buf->item, 16);
    return 0;
}

/**
 * 将状态保存到 buffer
 * 返回: buffer 指针（失败返回 nullptr）
 */
static inline buffer_t *save_state_to_buffer(uint64_t state[2]) {
    buffer_t *buf = allocate_buffer(16);
    if (!buf) {
        return nullptr;
    }
    memcpy(buf->item, state, 16);
    return buf;
}

// ==================== Efun 1: seed_random ====================

void f_seed_random() {
    auto *arg = sp - st_num_arg + 1;
    uint64_t state[2];
    int32_t min_val, max_val;
    buffer_t *input_buf = nullptr;
    int ret;

    if (st_num_arg != 3) {
        error("Bad argument number to seed_random(). Expect 3 arguments.\n");
    }

    if (arg[0].type != T_NUMBER && arg[0].type != T_STRING && arg[0].type != T_BUFFER) {
        error("Bad argument 1 to seed_random(). Expect int, string, or buffer.\n");
    }

    if (arg[1].type != T_NUMBER) {
        error("Bad argument 2 to seed_random(). Expect int.\n");
    }

    if (arg[2].type != T_NUMBER) {
        error("Bad argument 3 to seed_random(). Expect int.\n");
    }

    if (arg[0].type == T_BUFFER) {
        ret = load_state_from_buffer(arg[0].u.buf, state);
        if (ret != 0) {
            error("Bad argument 1 to seed_random(). Buffer size must be at least 16 bytes.\n");
        }
        input_buf = arg[0].u.buf;
    } else {
        ret = init_state_from_input(&arg[0], state);
        if (ret == -1) {
            error("Bad argument 1 to seed_random(). Negative seed not allowed.\n");
        } else if (ret == -2) {
            error("Bad argument 1 to seed_random(). Empty string not allowed.\n");
        } else if (ret == -3) {
            error("Bad argument 1 to seed_random(). Expect int, string, or buffer.\n");
        }
    }

    min_val = arg[1].u.number;
    max_val = arg[2].u.number;

    if (max_val < min_val) {
        error("Bad argument 3 to seed_random(). Max must be >= min.\n");
    }

    uint64_t range = (uint64_t)max_val - (uint64_t)min_val + 1;
    if (range > 0x100000000ULL) {
        error("Bad argument 2 & 3 to seed_random(). Range too large.\n");
    }

    uint64_t rand_val = xoroshiro128pp(state);
    int32_t value = min_val + (int32_t)(rand_val % range);

    if (input_buf) {
        memcpy(input_buf->item, state, 16);
    }

    free_svalue(sp, "f_seed_random");
    sp -= (st_num_arg - 1);
    sp->type = T_NUMBER;
    sp->u.number = value;
}

// ==================== Efun 2: seed_random_batch ====================

void f_seed_random_batch() {
    auto *arg = sp - st_num_arg + 1;
    uint64_t state[2];
    int32_t min_val, max_val, count;
    buffer_t *input_buf = nullptr;
    int ret;

    if (st_num_arg != 4) {
        error("Bad argument number to seed_random_batch(). Expect 4 arguments.\n");
    }

    if (arg[0].type != T_NUMBER && arg[0].type != T_STRING && arg[0].type != T_BUFFER) {
        error("Bad argument 1 to seed_random_batch(). Expect int, string, or buffer.\n");
    }

    if (arg[1].type != T_NUMBER) {
        error("Bad argument 2 to seed_random_batch(). Expect int.\n");
    }

    if (arg[2].type != T_NUMBER) {
        error("Bad argument 3 to seed_random_batch(). Expect int.\n");
    }

    if (arg[3].type != T_NUMBER) {
        error("Bad argument 4 to seed_random_batch(). Expect int.\n");
    }

    if (arg[0].type == T_BUFFER) {
        ret = load_state_from_buffer(arg[0].u.buf, state);
        if (ret != 0) {
            error("Bad argument 1 to seed_random_batch(). Buffer size must be at least 16 bytes.\n");
        }
        input_buf = arg[0].u.buf;
    } else {
        ret = init_state_from_input(&arg[0], state);
        if (ret == -1) {
            error("Bad argument 1 to seed_random_batch(). Negative seed not allowed.\n");
        } else if (ret == -2) {
            error("Bad argument 1 to seed_random_batch(). Empty string not allowed.\n");
        } else if (ret == -3) {
            error("Bad argument 1 to seed_random_batch(). Expect int, string, or buffer.\n");
        }
    }

    min_val = arg[1].u.number;
    max_val = arg[2].u.number;
    count = arg[3].u.number;

    if (max_val < min_val) {
        error("Bad argument 3 to seed_random_batch(). Max must be >= min.\n");
    }

    if (count <= 0) {
        error("Bad argument 4 to seed_random_batch(). Count must be > 0.\n");
    }

    if (count > 1000000) {
        error("Bad argument 4 to seed_random_batch(). Count must be <= 1000000.\n");
    }

    uint64_t range = (uint64_t)max_val - (uint64_t)min_val + 1;
    if (range > 0x100000000ULL) {
        error("Bad argument 2 & 3 to seed_random_batch(). Range too large.\n");
    }

    array_t *arr = allocate_array(count);
    if (!arr) {
        error("seed_random_batch: Failed to allocate array.\n");
    }

    for (int32_t i = 0; i < count; i++) {
        uint64_t rand_val = xoroshiro128pp(state);
        int32_t value = min_val + (int32_t)(rand_val % range);

        arr->item[i].type = T_NUMBER;
        arr->item[i].u.number = value;
    }

    if (input_buf) {
        memcpy(input_buf->item, state, 16);
    }

    free_svalue(sp, "f_seed_random_batch");
    sp->type = T_ARRAY;
    sp->u.arr = arr;
}

// ==================== Efun 3: seed_next ====================

void f_seed_next() {
    auto *arg = sp;
    uint64_t state[2];
    int ret;
    buffer_t *result_buf;

    if (st_num_arg != 1) {
        error("Bad argument number to seed_next(). Expect 1 argument.\n");
    }

    if (arg->type != T_NUMBER && arg->type != T_STRING && arg->type != T_BUFFER) {
        error("Bad argument 1 to seed_next(). Expect int, string, or buffer.\n");
    }

    if (arg->type == T_BUFFER) {
        ret = load_state_from_buffer(arg->u.buf, state);
        if (ret != 0) {
            error("Bad argument 1 to seed_next(). Buffer size must be at least 16 bytes.\n");
        }

        xoroshiro128pp(state);

        result_buf = save_state_to_buffer(state);
        if (!result_buf) {
            error("seed_next: Failed to allocate buffer.\n");
        }

        free_svalue(sp, "f_seed_next");
        sp->type = T_BUFFER;
        sp->u.buf = result_buf;
    } else {
        ret = init_state_from_input(arg, state);
        if (ret == -1) {
            error("Bad argument 1 to seed_next(). Negative seed not allowed.\n");
        } else if (ret == -2) {
            error("Bad argument 1 to seed_next(). Empty string not allowed.\n");
        } else if (ret == -3) {
            error("Bad argument 1 to seed_next(). Expect int, string, or buffer.\n");
        }

        result_buf = save_state_to_buffer(state);
        if (!result_buf) {
            error("seed_next: Failed to allocate buffer.\n");
        }

        free_svalue(sp, "f_seed_next");
        sp->type = T_BUFFER;
        sp->u.buf = result_buf;
    }
}
