#include "fw_image.h"
#include <string.h>

/* 读取 MCUboot 镜像头各字段并校验合法性.
 * 返回 TLV 区起始偏移 (hdr_size + img_size); 头非法时返回 0 且 *ok=false. */
static size_t fw_image_parse_header(const uint8_t *data, size_t len, bool *ok)
{
	*ok = false;

	/* MCUboot img header 至少 32B (magic 4 + 续 4 + hdr_size 2 + pad 2 +
	 * img_size 4 + flags 4 ...). 长度不足直接拒绝. */
	if (!data || len < 32) {
		return 0;
	}

	uint32_t magic = data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32_t)data[3] << 24);
	if (magic != IMG_MAGIC) {
		return 0;
	}

	uint16_t hdr_size = data[8] | (data[9] << 8);
	uint32_t img_size = data[12] | (data[13] << 8) | (data[14] << 16) | ((uint32_t)data[15] << 24);

	/* hdr_size 至少覆盖头部本身, 且为 4 对齐; img_size 不能超过剩余数据.
	 * 二者相加后必须能容纳至少 TLV info (4B). */
	if (hdr_size < 32 || (hdr_size & 0x3) || img_size > len) {
		return 0;
	}

	size_t tlv_off = (size_t)hdr_size + img_size;
	if (tlv_off + 4 > len) {
		return 0;
	}

	uint16_t tlv_magic = data[tlv_off] | (data[tlv_off + 1] << 8);
	if (tlv_magic != IMG_TLV_INFO_MAGIC) {
		return 0;
	}

	*ok = true;
	return tlv_off;
}

bool fw_image_validate_header(const uint8_t *data, size_t len)
{
	bool ok = false;
	fw_image_parse_header(data, len, &ok);
	return ok;
}

bool fw_image_extract_keyhash(const uint8_t *data, size_t len, uint8_t out_keyhash[IMG_KEYHASH_LEN])
{
	if (!out_keyhash) {
		return false;
	}

	bool ok = false;
	size_t tlv_off = fw_image_parse_header(data, len, &ok);
	if (!ok) {
		return false;
	}

	size_t off = tlv_off + 4;

	while (off + 4 <= len) {
		uint16_t tp = data[off] | (data[off + 1] << 8);
		uint16_t tlv_len = data[off + 2] | (data[off + 3] << 8);
		if (tp == 0 || tlv_len == 0 || off + 4 + tlv_len > len) {
			break;
		}
		if (tp == IMG_TLV_KEYHASH && tlv_len == IMG_KEYHASH_LEN) {
			memcpy(out_keyhash, data + off + 4, IMG_KEYHASH_LEN);
			return true;
		}
		off += 4 + tlv_len;
	}

	return false;
}
