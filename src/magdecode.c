/**
 * @file magdecode.c
 * @brief Bounds-checked MAG (MAKI02) image decoder implementation.
 *
 * Decodes the MAG flag and pixel streams, restores indexed images, and applies
 * MSX-specific SCREEN 6, SCREEN 10, and SCREEN 12 color interpretation without
 * display-aspect scaling.
 */

#include "magdecode.h"

#include <stdlib.h>
#include <string.h>

typedef struct MagHeader {
    size_t base;
    uint8_t machine_code;
    uint8_t machine_flag;
    uint8_t screen_mode;
    uint16_t sx, sy, ex, ey;
    uint32_t flaga_off, flagb_off, flagb_size, pixel_off, pixel_size;
    uint32_t aligned_x, aligned_width, row_bytes, groups_per_row;
} MagHeader;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int range_ok(size_t offset, size_t length, size_t size)
{
    return offset <= size && length <= size - offset;
}

static int parse_header(const uint8_t *data, size_t size, MagHeader *h)
{
    const uint8_t *endmark;
    const uint8_t *p;
    uint32_t align;
    size_t search_size;

    if (!data || size < 64 || memcmp(data, "MAKI02  ", 8) != 0)
        return MAG_ERR_FORMAT;

    search_size = size < 65536 ? size : 65536;
    endmark = (const uint8_t *)memchr(data, 0x1a, search_size);
    if (!endmark)
        return MAG_ERR_FORMAT;
    h->base = (size_t)(endmark - data) + 1;
    if (!range_ok(h->base, 32, size))
        return MAG_ERR_TRUNCATED;
    p = data + h->base;
    if (p[0] != 0)
        return MAG_ERR_FORMAT;

    h->machine_code = p[1];
    h->machine_flag = p[2];
    h->screen_mode = p[3];
    h->sx = le16(p + 4); h->sy = le16(p + 6);
    h->ex = le16(p + 8); h->ey = le16(p + 10);
    h->flaga_off = le32(p + 12); h->flagb_off = le32(p + 16);
    h->flagb_size = le32(p + 20); h->pixel_off = le32(p + 24);
    h->pixel_size = le32(p + 28);
    if (h->ex < h->sx || h->ey < h->sy)
        return MAG_ERR_FORMAT;

    align = h->screen_mode < 0x80 ? 8u : 4u;
    h->aligned_x = ((uint32_t)h->sx / align) * align;
    h->aligned_width = (((uint32_t)h->ex / align + 1) * align) - h->aligned_x;
    h->row_bytes = h->screen_mode < 0x80 ? h->aligned_width / 2 : h->aligned_width;
    if (!h->row_bytes || (h->row_bytes & 3u) != 0)
        return MAG_ERR_FORMAT;
    h->groups_per_row = h->row_bytes / 4;
    return MAG_OK;
}

int mag_probe(const uint8_t *data, size_t size)
{
    MagHeader h;
    return parse_header(data, size, &h) == MAG_OK;
}

void mag_free(MagImage *image)
{
    if (!image) return;
    free(image->pixels);
    free(image->comment);
    memset(image, 0, sizeof(*image));
}

int mag_decode(const uint8_t *data, size_t size, MagImage *image)
{
    static const int copy_x[16] = {0,2,4,8,0,2,0,2,4,0,2,4,0,2,4,0};
    static const int copy_y[16] = {0,0,0,0,1,1,2,2,2,4,4,4,8,8,8,16};
    MagHeader h;
    uint8_t *packed = NULL, *flags = NULL;
    const uint8_t *flaga, *flagb, *pixel, *palette;
    size_t flaga_size, flagb_pos = 0, pixel_pos = 0;
    size_t palette_count, palette_size, height, packed_size, pixel_count;
    unsigned msx_mode;
    uint8_t bit_mask = 0x80;
    size_t flaga_pos = 0, y, group, i;
    int rc;

    if (!image) return MAG_ERR_FORMAT;
    memset(image, 0, sizeof(*image));
    rc = parse_header(data, size, &h);
    if (rc != MAG_OK) return rc;

    height = (size_t)h.ey - h.sy + 1;
    palette_count = h.screen_mode < 0x80 ? 16 : 256;
    palette_size = palette_count * 3;
    flaga_size = ((size_t)h.groups_per_row * height + 7) / 8;
    if (!range_ok(h.base + 32, palette_size, size) ||
        !range_ok(h.base + h.flaga_off, flaga_size, size) ||
        !range_ok(h.base + h.flagb_off, h.flagb_size, size) ||
        !range_ok(h.base + h.pixel_off, h.pixel_size, size))
        return MAG_ERR_TRUNCATED;

    if (height > SIZE_MAX / h.row_bytes)
        return MAG_ERR_FORMAT;
    packed_size = height * h.row_bytes;
    packed = (uint8_t *)calloc(packed_size, 1);
    flags = (uint8_t *)calloc(h.groups_per_row, 1);
    if (!packed || !flags) { rc = MAG_ERR_MEMORY; goto done; }

    palette = data + h.base + 32;
    flaga = data + h.base + h.flaga_off;
    flagb = data + h.base + h.flagb_off;
    pixel = data + h.base + h.pixel_off;

    for (y = 0; y < height; ++y) {
        for (group = 0; group < h.groups_per_row; ++group) {
            uint8_t flag = 0;
            size_t out = y * h.row_bytes + group * 4;
            int half;
            if (flaga[flaga_pos] & bit_mask) {
                if (flagb_pos >= h.flagb_size) { rc = MAG_ERR_TRUNCATED; goto done; }
                flag = flagb[flagb_pos++];
            }
            flag ^= flags[group];
            flags[group] = flag;
            if ((bit_mask >>= 1) == 0) { bit_mask = 0x80; ++flaga_pos; }

            for (half = 0; half < 2; ++half) {
                unsigned code = half ? (flag & 15u) : (flag >> 4);
                size_t dst = out + (size_t)half * 2;
                if (code == 0) {
                    if (pixel_pos + 2 > h.pixel_size) { rc = MAG_ERR_TRUNCATED; goto done; }
                    packed[dst] = pixel[pixel_pos++];
                    packed[dst + 1] = pixel[pixel_pos++];
                } else {
                    int src_y = (int)y - copy_y[code];
                    int src_x = (int)(group * 4 + (size_t)half * 2) - copy_x[code];
                    size_t src;
                    if (src_y < 0 || src_x < 0 || src_x + 1 >= (int)h.row_bytes) {
                        rc = MAG_ERR_FORMAT; goto done;
                    }
                    src = (size_t)src_y * h.row_bytes + (size_t)src_x;
                    packed[dst] = packed[src]; packed[dst + 1] = packed[src + 1];
                }
            }
        }
    }

    msx_mode = h.machine_code == 3 ? h.machine_flag & 0xfcu : 0;
    image->width = (uint32_t)h.ex - h.sx + 1;
    image->height = (uint32_t)height;
    image->origin_x = h.sx; image->origin_y = h.sy;
    image->bits_per_pixel = h.screen_mode < 0x80 ? 4 : 8;
    image->palette_entries = (uint16_t)palette_count;
    for (i = 0; i < palette_count; ++i) {
        uint8_t r, g, b;
        /* MAG stores palette triples as G, R, B. */
        r = palette[i * 3 + 1]; g = palette[i * 3]; b = palette[i * 3 + 2];
        if (h.machine_code == 3) {
            r &= 0xe0; g &= 0xe0; b &= 0xe0;
            r = (uint8_t)(r | r >> 3 | r >> 6);
            g = (uint8_t)(g | g >> 3 | g >> 6);
            b = (uint8_t)(b | b >> 3 | b >> 6);
        }
        image->palette[i][0] = r; image->palette[i][1] = g; image->palette[i][2] = b;
    }

    if (msx_mode == 32 || msx_mode == 36 || msx_mode == 64 || msx_mode == 68) {
        /* MSX2+ SCREEN 10/12: four source bytes encode four YJK pixels. */
        uint32_t crop = h.screen_mode < 0x80 ? (h.sx - h.aligned_x) / 2 : h.sx - h.aligned_x;
        image->width = h.screen_mode < 0x80 ? (image->width + 1) / 2 : image->width;
        image->bits_per_pixel = 24;
        image->palette_entries = 0;
        pixel_count = (size_t)image->width * image->height;
        if (pixel_count > SIZE_MAX / 3) { rc = MAG_ERR_FORMAT; goto done; }
        image->pixels = (uint8_t *)malloc(pixel_count * 3);
        if (!image->pixels) { rc = MAG_ERR_MEMORY; goto done; }
        for (y = 0; y < height; ++y) {
            const uint8_t *line = packed + y * h.row_bytes + crop;
            for (i = 0; i < image->width; ++i) {
                unsigned yy = line[i] >> 3;
                int k, j, r, g, b;
                uint8_t *dst = image->pixels + (y * image->width + i) * 3;
                if ((msx_mode == 32 || msx_mode == 36) && (yy & 1u)) {
                    unsigned c = yy >> 1;
                    dst[0] = image->palette[c][0]; dst[1] = image->palette[c][1]; dst[2] = image->palette[c][2];
                    continue;
                }
                if ((i | 3u) >= image->width) {
                    r = g = b = (int)yy;
                } else {
                    size_t q = i & ~(size_t)3;
                    k = (line[q] & 7) | ((line[q + 1] & 7) << 3);
                    j = (line[q + 2] & 7) | ((line[q + 3] & 7) << 3);
                    if (k & 32) k -= 64;
                    if (j & 32) j -= 64;
                    r = (int)yy + j; g = (int)yy + k;
                    b = (5 * (int)yy - 2 * j - k + 2) >> 2;
                    if (r < 0) r = 0; else if (r > 31) r = 31;
                    if (g < 0) g = 0; else if (g > 31) g = 31;
                    if (b < 0) b = 0; else if (b > 31) b = 31;
                }
                dst[0] = (uint8_t)((r << 3) | (r >> 2));
                dst[1] = (uint8_t)((g << 3) | (g >> 2));
                dst[2] = (uint8_t)((b << 3) | (b >> 2));
            }
        }
    } else if (msx_mode == 96 || msx_mode == 100) {
        /* MSX2 SCREEN 6: each compressed byte contains four 2-bit pixels. */
        uint32_t crop = h.screen_mode < 0x80 ? (h.sx - h.aligned_x) / 2 : h.sx - h.aligned_x;
        uint32_t source_bytes = (image->width + 1) / 2;
        image->width = source_bytes * 4;
        image->bits_per_pixel = 4;
        image->palette_entries = 16;
        pixel_count = (size_t)image->width * image->height;
        image->pixels = (uint8_t *)malloc(pixel_count);
        if (!image->pixels) { rc = MAG_ERR_MEMORY; goto done; }
        for (y = 0; y < height; ++y) {
            const uint8_t *line = packed + y * h.row_bytes + crop;
            for (i = 0; i < image->width; ++i)
                image->pixels[y * image->width + i] = (uint8_t)((line[i >> 2] >> ((~i & 3) << 1)) & 3);
        }
    } else {
        pixel_count = (size_t)image->width * image->height;
        image->pixels = (uint8_t *)malloc(pixel_count);
        if (!image->pixels) { rc = MAG_ERR_MEMORY; goto done; }
        {
        uint32_t crop = h.sx - h.aligned_x;
        for (y = 0; y < height; ++y) {
            size_t x;
            for (x = 0; x < image->width; ++x) {
                uint32_t ax = crop + (uint32_t)x;
                uint8_t v = packed[y * h.row_bytes + (image->bits_per_pixel == 4 ? ax / 2 : ax)];
                image->pixels[y * image->width + x] = image->bits_per_pixel == 4
                    ? ((ax & 1) ? (v & 15u) : (v >> 4)) : v;
            }
        }
        }
    }
    if (h.base > 32) {
        size_t comment_len = h.base - 32 - 1;
        image->comment = (char *)malloc(comment_len + 1);
        if (!image->comment) { rc = MAG_ERR_MEMORY; goto done; }
        memcpy(image->comment, data + 31, comment_len);
        image->comment[comment_len] = '\0';
    }
    rc = MAG_OK;

done:
    free(packed); free(flags);
    if (rc != MAG_OK) mag_free(image);
    return rc;
}
