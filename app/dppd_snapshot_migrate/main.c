#include <errno.h>
#include <stdbool.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dppd/persistence.h"

static void print_usage(const char *program)
{
    fprintf(stderr,
            "用法：%s --input <v1快照> --output <v2快照> --install-port <端口ID>\n"
            "\n"
            "v1 快照没有保存规则安装端口。本工具会把 --install-port 应用于\n"
            "全部规则，再以原子方式写出 v2 快照；输入文件不会被修改。\n",
            program);
}

static int parse_port_id(const char *text, uint16_t *port_id)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0' || text[0] == '-')
        return -EINVAL;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT16_MAX)
        return -EINVAL;
    *port_id = (uint16_t)value;
    return 0;
}

/*
 * 输入和输出一旦是同一 inode，rename 写回会破坏唯一的旧快照。因此即使用户
 * 使用不同的路径拼写、硬链接或符号链接，也明确拒绝原地迁移。
 */
static int ensure_distinct_files(const char *input, const char *output)
{
    struct stat input_metadata;
    struct stat output_metadata;

    if (stat(input, &input_metadata) != 0)
        return -errno;
    if (stat(output, &output_metadata) != 0) {
        if (errno == ENOENT)
            return 0;
        return -errno;
    }
    if (input_metadata.st_dev == output_metadata.st_dev &&
        input_metadata.st_ino == output_metadata.st_ino)
        return -EEXIST;
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option long_options[] = {
        { "input", required_argument, NULL, 'i' },
        { "output", required_argument, NULL, 'o' },
        { "install-port", required_argument, NULL, 'p' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    struct dppd_persisted_snapshot snapshot;
    struct dppd_rule_repository repository;
    const char *input = NULL;
    const char *output = NULL;
    uint16_t install_port_id = 0;
    uint32_t capacity;
    int option;
    int rc;
    uint32_t i;
    bool has_port = false;

    memset(&snapshot, 0, sizeof(snapshot));
    memset(&repository, 0, sizeof(repository));
    while ((option = getopt_long(argc, argv, "i:o:p:h", long_options,
                                 NULL)) != -1) {
        switch (option) {
        case 'i':
            input = optarg;
            break;
        case 'o':
            output = optarg;
            break;
        case 'p':
            rc = parse_port_id(optarg, &install_port_id);
            if (rc != 0) {
                fprintf(stderr, "无效的安装端口：%s\n", optarg);
                return EXIT_FAILURE;
            }
            has_port = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (optind != argc || input == NULL || output == NULL || !has_port) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    rc = ensure_distinct_files(input, output);
    if (rc != 0) {
        if (rc == -EEXIST) {
            fprintf(stderr, "拒绝迁移：输入与输出不能是同一文件\n");
        } else {
            fprintf(stderr, "检查迁移路径失败：%s\n", strerror(-rc));
        }
        return EXIT_FAILURE;
    }
    rc = dppd_persistence_load_v1_for_migration(input, &snapshot);
    if (rc != 0) {
        fprintf(stderr, "读取 v1 快照失败：%s\n", strerror(-rc));
        return EXIT_FAILURE;
    }

    /* v1 从未记录端口归属，故只支持用户确认过的统一端口映射。 */
    for (i = 0; i < snapshot.count; ++i)
        snapshot.rules[i].install_port_id = install_port_id;
    capacity = snapshot.count == 0 ? 1 : snapshot.count;
    rc = dppd_rule_repository_init(&repository, capacity);
    if (rc == 0) {
        /* restore 原样保留 rule 和 repository generation，不重新分配版本号。 */
        rc = dppd_rule_repository_restore(&repository, snapshot.rules,
                                          snapshot.count,
                                          snapshot.repository_generation);
    }
    if (rc == 0)
        rc = dppd_persistence_save(output, &repository);
    if (rc != 0) {
        fprintf(stderr, "写入 v2 快照失败：%s\n", strerror(-rc));
        dppd_rule_repository_destroy(&repository);
        dppd_persisted_snapshot_destroy(&snapshot);
        return EXIT_FAILURE;
    }
    printf("迁移完成：%" PRIu32 " 条规则写入 %s（安装端口 %" PRIu16 "）\n",
           snapshot.count, output, install_port_id);
    dppd_rule_repository_destroy(&repository);
    dppd_persisted_snapshot_destroy(&snapshot);
    return EXIT_SUCCESS;
}
