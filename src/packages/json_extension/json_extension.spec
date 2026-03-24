/**
 * 将 LPC 值编码为 JSON 字符串。
 *
 * @description
 * 支持将基础标量、数组、mapping 等可序列化的 LPC 数据结构递归转换为
 * JSON 文本，适合做网关传输、文件落盘或调试输出。
 *
 * @param value 待编码的 LPC 值。
 * @returns 编码成功时返回 JSON 字符串；当值无法序列化，或内部写出/分配
 * 失败时返回 `0`。
 */
string json_encode(mixed);
/**
 * 将 JSON 字符串解码为 LPC 值。
 *
 * @description
 * 解析标准 JSON 文本，并转换回 LPC 可用的 mixed 值。常见的对象会被转成
 * mapping，数组会被转成 LPC 数组，数字、布尔和字符串会映射为对应基础类型。
 *
 * @param json 待解析的 JSON 文本。
 * @returns 解析成功时返回转换后的 LPC 值；当输入为空或 JSON 非法时返回 `0`。
 */
mixed json_decode(string);
/**
 * 将 LPC 值编码为 JSON 并写入文件。
 *
 * @description
 * 该函数会先对目标路径做驱动层安全校验，再将传入值序列化为 JSON 文本并
 * 覆盖写入文件。适合保存配置快照、缓存数据或运行时状态。
 *
 * @param path 目标文件路径，必须通过驱动的写权限与合法路径检查。
 * @param value 待写入的 LPC 值。
 * @returns 写入成功返回 `1`；路径非法、文件打开失败、序列化失败或写入失败
 * 时返回 `0`。
 */
int write_json(string, mixed);
/**
 * 从文件读取 JSON 并解码为 LPC 值。
 *
 * @description
 * 该函数会先校验读取路径，再加载文件内容并按 JSON 解析。适合读取配置、
 * 持久化状态或外部同步的 JSON 数据。
 *
 * @param path 待读取的 JSON 文件路径，必须通过驱动的读权限与合法路径检查。
 * @returns 读取并解析成功时返回转换后的 LPC 值；当路径非法、文件无法打开、
 * 文件内容过大、读取失败或 JSON 非法时返回 `0`。
 */
mixed read_json(string);
/**
 * 按 mapping 的值进行通用排序，并返回排好序的 key 数组。
 *
 * @description
 * 该函数会遍历 mapping，把每个 value 转换为可比较的排序权重后进行排序，
 * 最终返回“按 value 顺序排列后的 key 列表”。
 *
 * 支持的 value 类型：
 * - `int`
 * - `float`
 * - 可解析为数字的 `string`
 *
 * 其他无法参与数值比较的值会落到最低权重。若两个 value 的排序权重相同，
 * 会再按 key 内容做稳定的确定性比较，避免结果顺序漂移。
 *
 * @param data 待排序的 mapping。
 * @param flag 排序方向标志。`0` 或省略表示升序；`1` 表示降序。
 * @returns 一个 key 数组，数组元素顺序与对应 value 的排序结果一致。
 */
mixed *sort_mapping(mapping, int | void);
/**
 * 按 mapping 的整数值进行快速排序，并返回排好序的 key 数组。
 *
 * @description
 * 这是面向整数场景的高性能排序版本，适合大规模数值 mapping 的快速排序。
 * 与 `sort_mapping()` 一样，返回值是“按 value 顺序排列后的 key 列表”，
 * 不是排序后的 value 数组。
 *
 * 该函数只按 `int` 值参与排序；非整数 value 会按 `0` 处理。
 *
 * @param data 待排序的 mapping。
 * @param flag 排序方向标志。`0` 或省略表示升序；`1` 表示降序。
 * @returns 一个 key 数组，数组元素顺序与对应整数 value 的排序结果一致。
 */
mixed *sort_mapping_int(mapping, int | void);
