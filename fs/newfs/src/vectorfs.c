#include "vectorfs.h"
#include "bonus.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/******************************************************************************
* SECTION: VectorFS 实现说明
*******************************************************************************/
/* 1. 教师提供按查询词（query）缓存的 vectorfs_cache_get(query, &results, &count)：
 *    - 缓存命中时直接返回之前算好的只读结果数组；未命中时自动调用你实现的 vectorfs_search。
 *    - 当 /documents 下的普通文件发生写入、删除、改名或卸载时，教师路由层会自动清空缓存。
 * 2. 每个结果项 struct vectorfs_result 包含：
 *    - source : 相对于 /documents 的文档路径（如 "os/inode.txt"，不带 "/documents/" 前缀）
 *    - rate   : 原始余弦相似度（double，范围 [-1.0, 1.0]）
 *    - rank   : 从 0 开始的排名下标（0 .. count-1），第一名对应后缀 "__00"，第十名对应 "__09"
 * 3. 三个 FUSE 回调（getattr/readdir/readlink）借用缓存结果后只读使用，切勿修改或 free(results)。 */

/******************************************************************************
* SECTION: 搜索排序与结果校验辅助函数
*******************************************************************************/
struct search_candidate {
    const char *source; // 借用文档数组中的相对路径，释放文档前复制到最终结果
    double rate;        // 与该路径绑定的原始相似度
};

// 分数高的在前，同分时按完整相对路径升序
static int compare_candidates(const void *left, const void *right) {
    const struct search_candidate *a = left;
    const struct search_candidate *b = right;
    if (a->rate > b->rate)
        return -1;
    if (a->rate < b->rate)
        return 1;
    return strcmp(a->source, b->source);
}

// 核对结果确实存在且名称与缓存一致，返回的指针由教师缓存持有，调用方不释放
static int lookup_result(const char *path, const char *query, size_t rank,
                         const struct vectorfs_result **result) {
    const struct vectorfs_result *results;
    size_t count;
    CHECK(vectorfs_cache_get(query, &results, &count));
    if (rank >= count)
        return -ENOENT;

    char name[VECTORFS_RESULT_NAME_MAX];
    CHECK(vectorfs_result_name(&results[rank], name, sizeof(name)));
    // 路径解析允许末尾斜杠，比较时只取最后一级结果名
    const char *end = path + strlen(path);
    if (end > path && end[-1] == '/')
        end--;
    const char *start = end;
    while (start > path && start[-1] != '/')
        start--;
    if ((size_t)(end - start) != strlen(name) || memcmp(start, name, end - start))
        return -ENOENT;

    *result = &results[rank];
    return 0;
}

/******************************************************************************
* SECTION: 附加题三 VectorFS 函数实现
*******************************************************************************/
/**
 * @brief 当查询缓存未命中时，计算查询词与所有普通文档的相似度并按排名填入前十项
 *
 * @param query 已完成百分号解码的查询文本（如 "file system"）
 * @param results 教师缓存预分配好的容量为 VECTORFS_TOP_K（10）的结果数组，直接填入，不要 malloc/free
 * @param count 输出实际填入的结果数量，即 min(文档总数, VECTORFS_TOP_K)；无文档时设为 0
 * @return int 成功返回 0，失败时释放本函数自己申请的资源并返回负错误码
 */
int vectorfs_search(const char *query,
                    struct vectorfs_result results[VECTORFS_TOP_K], size_t *count) {
    /* 按以下步骤完成文档向量匹配与排序：
     * Step 1: 调用 vectorfs_load_documents(&docs, &doc_count) 加载 /documents 下的所有普通文档。
     *         若 doc_count == 0，设置 *count = 0，调用 vectorfs_documents_free(docs, doc_count) 后返回 0。
     * Step 2: 调用 vectorfs_embed(query, strlen(query), query_vec) 计算查询词的 8 维向量。
     * Step 3: 遍历 docs[0 .. doc_count-1]，用 vectorfs_similarity(query_vec, docs[i].embedding)
     *         计算每个文档的相似度 rate，并与该文档的 docs[i].relative_path 保持配对。
     * Step 4: 按【rate 降序、同分时按 relative_path 字典序（strcmp）升序】对所有文档排序（可用 qsort）。
     * Step 5: 取前 k = min(doc_count, VECTORFS_TOP_K) 项填入 results[0 .. k-1]：
     *         - 将 relative_path 拷贝到 results[i].source（必须拷贝字符串内容，不能直接赋指针）；
     *         - results[i].rate = 对应的相似度；
     *         - results[i].rank = i（从 0 开始依次为 0, 1, ..., k-1）；
     *         - 设置 *count = k。
     * Step 6: 调用 vectorfs_documents_free(docs, doc_count) 释放文档数组（及你自己的临时排序数组），返回 0。 */
    if (!query || !results || !count)
        return -EINVAL;
    *count = 0;

    struct vectorfs_document *docs = NULL;
    size_t doc_count = 0;
    CHECK(vectorfs_load_documents(&docs, &doc_count));
    if (doc_count == 0) {
        vectorfs_documents_free(docs, doc_count);
        return 0;
    }

    float query_vec[VECTORFS_DIMENSION];
    int result = vectorfs_embed(query, strlen(query), query_vec);
    if (result < 0) {
        vectorfs_documents_free(docs, doc_count);
        return result;
    }
    struct search_candidate *candidates = calloc(doc_count, sizeof(*candidates));
    if (!candidates) {
        vectorfs_documents_free(docs, doc_count);
        return -ENOMEM;
    }
    for (size_t i = 0; i < doc_count; i++) {
        candidates[i].source = docs[i].relative_path;
        candidates[i].rate = vectorfs_similarity(query_vec, docs[i].embedding);
    }
    qsort(candidates, doc_count, sizeof(*candidates), compare_candidates);

    size_t top = doc_count < VECTORFS_TOP_K ? doc_count : VECTORFS_TOP_K;
    for (size_t i = 0; i < top; i++) {
        size_t length = strlen(candidates[i].source);
        if (length >= sizeof(results[i].source)) {
            free(candidates);
            vectorfs_documents_free(docs, doc_count);
            return -ENAMETOOLONG;
        }
        memcpy(results[i].source, candidates[i].source, length + 1);
        results[i].rate = candidates[i].rate;
        results[i].rank = i;
    }
    *count = top;

    free(candidates);
    vectorfs_documents_free(docs, doc_count);
    return 0;
}

/**
 * @brief 获取搜索目录（/.search、/.search/<query>）或结果软链接的文件属性
 *
 * @param path 相对于挂载根的完整路径
 * @param st 用于返回文件属性的结构体
 * @return int 成功返回 0，失败返回负错误码（如 -ENOENT）
 */
int vectorfs_getattr(const char *path, struct stat *st) {
    /* 按以下步骤填充虚拟目录或结果软链接的 struct stat：
     * Step 1: 先用 memset(st, 0, sizeof(*st)) 清零属性结构体，再调用
     *         int kind = vectorfs_parse_path(path, &query, &rate, &rank);
     *         若 kind < 0 直接返回 kind。注意：后续所有返回分支都必须 free(query)！
     * Step 2: 若 kind == 0（"/.search"）或 kind == 1（"/.search/<query>"）：
     *         它们是只读目录，无需触发搜索，直接设置：
     *         st->st_mode = S_IFDIR | 0555; st->st_nlink = 2;
     *         然后 free(query) 并返回 0。
     * Step 3: 若 kind == 2（"/.search/<query>/<rate>__<rank>"）：
     *         - 调用 vectorfs_cache_get(query, &results, &count) 获取缓存结果；
     *         - 检查 rank < count，并核对 results[rank] 格式化后的名称（调用 vectorfs_result_name）
     *           与路径中的 <rate>__<rank> 是否一致；若越界或不一致，free(query) 后返回 -ENOENT；
     *         - 调用 vectorfs_result_target(&results[rank], target, sizeof(target)) 生成软链接目标路径；
     *         - 设置软链接属性：st->st_mode = S_IFLNK | 0777; st->st_nlink = 1;
     *           st->st_size = strlen(target);（注意：st_size 是软链接目标路径的字节长度，不是原文档大小！）
     *         - 最后 free(query) 并返回 0。 */
    if (!st)
        return -EINVAL;
    memset(st, 0, sizeof(*st));
    char *query = NULL;
    double rate;
    size_t rank;
    int kind = vectorfs_parse_path(path, &query, &rate, &rank);
    if (kind < 0)
        return kind;

    int result = 0;
    if (kind == 0 || kind == 1) {
        // 查询目录动态存在，不需要分配 inode，也不必为了获取目录属性执行搜索
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
    } else {
        const struct vectorfs_result *entry;
        result = lookup_result(path, query, rank, &entry);
        if (result == 0) {
            char target[VECTORFS_TARGET_MAX];
            result = vectorfs_result_target(entry, target, sizeof(target));
            if (result == 0) {
                st->st_mode = S_IFLNK | 0777;
                st->st_nlink = 1;
                st->st_size = strlen(target); // 软链接大小是目标路径长度，不是原文档大小
            }
        }
    }
    free(query);
    return result;
}

/**
 * @brief 遍历搜索根目录或查询目录，调用 filler 填充动态目录项
 *
 * @param path 相对于挂载根的完整路径
 * @param buf FUSE 提供的目录项缓冲区，原样传给 filler 的第一个参数
 * @param filler FUSE 目录项填充回调：filler(buf, name, NULL, next_offset)
 * @param offset 本次遍历起始的目录项位置序号（包含 "." 和 ".." 在内的总序号，从 0 开始）
 * @param fi 文件信息，可忽略
 * @return int 成功返回 0，失败返回负错误码
 */
int vectorfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                     off_t offset, struct fuse_file_info *fi) {
    /* 按以下步骤实现动态目录遍历：
     * Step 1: 若 offset < 0 返回 -EINVAL。调用 kind = vectorfs_parse_path(path, &query, &rate, &rank)：
     *         - 若 kind < 0 返回 kind；
     *         - 若 kind == 2（结果软链接不是目录），free(query) 后返回 -ENOTDIR。
     * Step 2: 若 kind == 0（搜索根目录 "/.search"）：
     *         只列出 "."（序号 0，next_offset 传 1）和 ".."（序号 1，next_offset 传 2），不列历史查询。
     * Step 3: 若 kind == 1（查询目录 "/.search/<query>"）：
     *         - 调用 vectorfs_cache_get(query, &results, &count) 获取结果数组；
     *         - 目录项总序号安排：序号 0 为 "."，序号 1 为 ".."，序号 i + 2 对应 results[i]（0 <= i < count）；
     *         - 对每个 results[i]，调用 vectorfs_result_name(&results[i], name, sizeof(name)) 生成目录项名；
     *         - 从 offset 对应的序号开始依次调用 filler(buf, name, NULL, 当前序号 + 1)：
     *           一旦 filler 返回非 0 值（表示缓冲区已满），立即停止循环，free(query) 并返回 0。 */
    (void)fi;
    if (offset < 0)
        return -EINVAL;
    char *query = NULL;
    double rate;
    size_t rank;
    int kind = vectorfs_parse_path(path, &query, &rate, &rank);
    if (kind < 0)
        return kind;
    if (kind == 2) {
        free(query);
        return -ENOTDIR;
    }

    const struct vectorfs_result *results = NULL;
    size_t count = 0;
    int result = 0;
    if (kind == 1) {
        result = vectorfs_cache_get(query, &results, &count);
        if (result < 0) {
            free(query);
            return result;
        }
    }

    // 序号 0、1 为 . 和 ..，之后是结果；offset 和下一位置都使用这套总序号
    for (off_t pos = offset; pos < (off_t)count + 2; pos++) {
        char name[VECTORFS_RESULT_NAME_MAX];
        const char *entry_name;
        if (pos == 0) {
            entry_name = ".";
        } else if (pos == 1) {
            entry_name = "..";
        } else {
            result = vectorfs_result_name(&results[pos - 2], name, sizeof(name));
            if (result < 0)
                break;
            entry_name = name;
        }
        if (filler(buf, entry_name, NULL, pos + 1))
            break; // 缓冲区满是正常情况，下一次会根据 offset 继续读取
    }
    free(query);
    return result;
}

/**
 * @brief 读取搜索结果软链接所指向的目标相对路径（"../../documents/<source>"）
 *
 * @param path 相对于挂载根的完整结果路径（如 "/.search/a/1.000000__00"）
 * @param buf 用于返回软链接目标路径字符串的缓冲区
 * @param size buf 的字节容量（包含末尾的 '\0' 结束符）
 * @return int 成功返回 0（FUSE2 约定返回 0，不返回字节数），失败返回负错误码
 */
int vectorfs_readlink(const char *path, char *buf, size_t size) {
    /* 按以下步骤填充软链接目标路径：
     * Step 1: 若 !buf || size == 0 返回 -EINVAL。
     *         调用 kind = vectorfs_parse_path(path, &query, &rate, &rank)：
     *         - 若 kind < 0 返回 kind；
     *         - 若 kind == 0 || kind == 1（目录不是软链接），free(query) 后返回 -EINVAL。
     * Step 2: 当 kind == 2 时，调用 vectorfs_cache_get(query, &results, &count) 获取缓存结果：
     *         检查 rank < count，并核对 results[rank] 的名称（vectorfs_result_name）与路径中的
     *         <rate>__<rank> 是否一致；若不存在或分数不匹配，free(query) 后返回 -ENOENT。
     * Step 3: 调用 vectorfs_result_target(&results[rank], target, sizeof(target)) 生成
     *         "../../documents/<source>" 目标路径字符串。
     * Step 4: 将 target 拷贝到 buf 中（最多拷贝 size - 1 个字节，并在末尾补 '\0'），
     *         free(query) 后返回 0（注意：不要返回原文档内容，也不要返回字符串长度）。 */
    if (!buf || size == 0)
        return -EINVAL;
    char *query = NULL;
    double rate;
    size_t rank;
    int kind = vectorfs_parse_path(path, &query, &rate, &rank);
    if (kind < 0)
        return kind;
    if (kind != 2) {
        free(query);
        return -EINVAL;
    }

    const struct vectorfs_result *entry;
    int result = lookup_result(path, query, rank, &entry);
    if (result == 0) {
        char target[VECTORFS_TARGET_MAX];
        result = vectorfs_result_target(entry, target, sizeof(target));
        if (result == 0) {
            size_t count = strlen(target);
            if (count >= size)
                count = size - 1;
            memcpy(buf, target, count);
            buf[count] = '\0';
        }
    }
    free(query);
    return result;
}
