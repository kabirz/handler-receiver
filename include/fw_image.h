#ifndef FW_IMAGE_H
#define FW_IMAGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* MCUboot 签名镜像 magic + TLV 解析, 用于提取镜像内 KEYHASH (32B). */
#define IMG_MAGIC 0x96f3b83d
#define IMG_TLV_INFO_MAGIC 0x6907
#define IMG_TLV_KEYHASH 0x01
#define IMG_KEYHASH_LEN 32

/*
 * 校验 MCUboot 镜像头 (magic + 头长度 + 镜像长度 + TLV info magic).
 * 用于在升级前拒绝非固件文件 (任意二进制/文本等).
 * @param data  整段固件数据
 * @param len   固件长度
 * @return true=合法 MCUboot 镜像; false=非 MCUboot 镜像/损坏/过短
 */
bool fw_image_validate_header(const uint8_t *data, size_t len);

/*
 * 从 MCUboot 签名固件 (zephyr.signed.bin) 提取 KEYHASH TLV (32B).
 * @param data       整段固件数据
 * @param len        固件长度
 * @param out_keyhash 输出 32B keyhash
 * @return true=成功; false=格式错误/无 KEYHASH TLV
 */
bool fw_image_extract_keyhash(const uint8_t *data, size_t len, uint8_t out_keyhash[IMG_KEYHASH_LEN]);

#endif /* FW_IMAGE_H */
