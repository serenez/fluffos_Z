/* Xoroshiro128++ 种子系统包接口定义 */
/* Final v3.0 - 极简直观版 */

/* 生成单个随机数（自动适配 int|string|buffer） */
int seed_random(int | string | buffer, int, int);

/* 批量生成随机数（自动适配 int|string|buffer） */
int *seed_random_batch(int | string | buffer, int, int, int);

/* 获取下一个状态 buffer（自动适配 int|string|buffer） */
buffer seed_next(int | string | buffer);
