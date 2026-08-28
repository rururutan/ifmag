/**
 * @file ifmag.c
 * @brief Susie 32/64-bit plug-in adapter for MAG (MAKI02) images.
 *
 * Implements format detection, image-information retrieval, and DIB creation.
 * MAG parsing and decompression are delegated to magdecode.c, while the common
 * Susie entry-point layer is supplied by the shared SPI sources.
 */

#define STRICT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "spibase.h"
#include "magdecode.h"

/* The shared SPI I/O header predates this helper and lacks its declaration. */
LONG_PTR SpiGetFileSize(SPI_FILE *fp);

#define IFMAG_VERSION "0.30"

const int NumInfo = 4;
const LPCSTR PluginInfo[] = {
    "00IN",
    "MAG to DIB filter ver." IFMAG_VERSION " (C) Ru^3",
    "*.mag",
    "MAG"
};

int IsSupportedFormat(LPBYTE buf, DWORD rbytes, LPCSTR filename)
{
    (void)filename;
    return mag_probe(buf, rbytes);
}

static int read_all(SPI_FILE *fp, uint8_t **out, size_t *out_size)
{
    LONG_PTR file_size = SpiGetFileSize(fp);
    size_t size;
    uint8_t *data;
    if (file_size <= 0 || (ULONG_PTR)file_size > 0xffffffffu)
        return SPI_ERROR_BROKEN_DATA;
    size = (size_t)file_size;
    data = (uint8_t *)malloc(size);
    if (!data) return SPI_ERROR_ALLOCATE_MEMORY;
    SpiSeek(fp, 0, FILE_BEGIN);
    if (SpiRead(data, (DWORD)size, fp) != size) {
        free(data); return SPI_ERROR_FILE_READ;
    }
    *out = data; *out_size = size;
    return SPI_ERROR_SUCCESS;
}

static int decode_error(int rc)
{
    return rc == MAG_ERR_MEMORY ? SPI_ERROR_ALLOCATE_MEMORY : SPI_ERROR_BROKEN_DATA;
}

int GetImageInfo(SPI_FILE *fp, PictureInfo *lpInfo)
{
    uint8_t *data = NULL;
    size_t size = 0;
    MagImage image;
    int ret = read_all(fp, &data, &size);
    if (ret != SPI_ERROR_SUCCESS) return ret;
    ret = mag_decode(data, size, &image);
    free(data);
    if (ret != MAG_OK) return decode_error(ret);
    SpiSetPictureInfo(lpInfo, image.width, image.height, image.bits_per_pixel,
                      image.origin_x, image.origin_y, 0, 0, NULL);
    if (image.comment && image.comment[0]) {
        size_t n = strlen(image.comment) + 1;
        LPBYTE text;
        if (SpiAllocBuffer(&lpInfo->hInfo, &text, n) == SPI_ERROR_SUCCESS) {
            memcpy(text, image.comment, n);
            SpiUnlockBuffer(&lpInfo->hInfo);
        }
    }
    mag_free(&image);
    return SPI_ERROR_SUCCESS;
}

int GetImage(SPI_FILE *fp, HANDLE *pHBInfo, HANDLE *pHBImg,
             SPIPROC progress, LONG_PTR user)
{
    uint8_t *data = NULL;
    size_t size = 0, y, x;
    MagImage image;
    LPBITMAPINFO bmi = NULL;
    LPBYTE bits = NULL;
    DWORD rowbytes = 0;
    int ret = read_all(fp, &data, &size);
    if (ret != SPI_ERROR_SUCCESS) return ret;
    if (progress && progress(10, 100, user)) { free(data); return SPI_ERROR_CANCEL_EXPAND; }
    ret = mag_decode(data, size, &image);
    free(data);
    if (ret != MAG_OK) return decode_error(ret);
    if (progress && progress(70, 100, user)) { mag_free(&image); return SPI_ERROR_CANCEL_EXPAND; }

    ret = SpiInitBitmap((HLOCAL *)pHBInfo, &bmi, (HLOCAL *)pHBImg, &bits, &rowbytes,
                        image.width, image.height, image.bits_per_pixel,
                        image.palette_entries, 0, 0);
    if (ret != SPI_ERROR_SUCCESS) { mag_free(&image); return ret; }
    for (x = 0; x < image.palette_entries; ++x) {
        bmi->bmiColors[x].rgbRed = image.palette[x][0];
        bmi->bmiColors[x].rgbGreen = image.palette[x][1];
        bmi->bmiColors[x].rgbBlue = image.palette[x][2];
        bmi->bmiColors[x].rgbReserved = 0;
    }
    for (y = 0; y < image.height; ++y) {
        const uint8_t *src = image.pixels + (image.height - 1 - y) * image.width *
                             (image.bits_per_pixel == 24 ? 3 : 1);
        uint8_t *dst = bits + y * rowbytes;
        if (image.bits_per_pixel == 24) {
            for (x = 0; x < image.width; ++x) {
                dst[x * 3] = src[x * 3 + 2];
                dst[x * 3 + 1] = src[x * 3 + 1];
                dst[x * 3 + 2] = src[x * 3];
            }
        } else if (image.bits_per_pixel == 8) {
            memcpy(dst, src, image.width);
        } else {
            for (x = 0; x < image.width; x += 2) {
                uint8_t right = x + 1 < image.width ? src[x + 1] : 0;
                dst[x / 2] = (uint8_t)((src[x] << 4) | right);
            }
        }
    }
    mag_free(&image);
    SpiUnlockBuffer(pHBInfo); SpiUnlockBuffer(pHBImg);
    if (progress) progress(100, 100, user);
    return SPI_ERROR_SUCCESS;
}
