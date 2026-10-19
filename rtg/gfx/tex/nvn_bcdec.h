// GPU 不支持 BC 格式时，将压缩纹理解码为上传使用的像素格式。
// Decode BC textures on the CPU when the GPU lacks native format support.
#pragma once
#include <stddef.h>
#include <stdint.h>

// NVN 格式是 BC → 返回 1，*mtl_pf = 解压后的 Metal 像素格式、*bpp = 解压后每像素字节；不是 BC 返回 0
// NVN format is BC → returns 1, *mtl_pf = Metal pixel format after decompression, *bpp = bytes per pixel after decompression; returns 0 if not BC
int nvn_bc_unpacked(uint32_t nvn_fmt, uint32_t *mtl_pf, int *bpp);
// 解一片 w×h 像素：src 按块行排（每块行 pitch 字节），dst 每像素行 dpitch 字节。成功 0
// Decode a w×h pixel region: src is laid out by block rows (pitch bytes per block row), dst has dpitch bytes per pixel row. Returns 0 on success
int nvn_bc_decode(uint32_t nvn_fmt, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch);
/* One BC7 block, including uncommon modes handled by the reference decoder. */
void nvn_bc7_decode_block(const uint8_t *source,uint8_t *pixels,size_t pitch);
/* Upload destinations may be uncached/write-combined mapped GPU memory. */
int nvn_bc_decode_upload(uint32_t nvn_fmt,const uint8_t *src,size_t pitch,uint32_t w,uint32_t h,uint8_t *dst,size_t dpitch);
