#ifndef MAGDECODE_H
#define MAGDECODE_H

#include <stddef.h>
#include <stdint.h>

typedef struct MagImage {
    uint32_t width;
    uint32_t height;
    uint16_t origin_x;
    uint16_t origin_y;
    uint8_t bits_per_pixel;
    uint16_t palette_entries;
    uint8_t palette[256][3]; /* RGB */
    uint8_t *pixels;        /* top-down, one palette index per byte */
    char *comment;
} MagImage;

enum {
    MAG_OK = 0,
    MAG_ERR_FORMAT = 1,
    MAG_ERR_TRUNCATED = 2,
    MAG_ERR_MEMORY = 3
};

int mag_probe(const uint8_t *data, size_t size);
int mag_decode(const uint8_t *data, size_t size, MagImage *image);
void mag_free(MagImage *image);

#endif
