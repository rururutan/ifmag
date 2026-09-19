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
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "spibase.h"
#include "magdecode.h"

/* The shared SPI I/O header predates this helper and lacks its declaration. */
LONG_PTR SpiGetFileSize(SPI_FILE *fp);

#define IFMAG_VERSION "0.40"

const int NumInfo = 4;
const LPCSTR PluginInfo[] = {
    "00IN",
    "MAG to DIB filter ver." IFMAG_VERSION " (C) Ru^3",
    "*.mag;*.max",
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

/** Store a MAG Shift-JIS comment in the encoding required by the Susie API. */
static int set_comment(PictureInfo *info, const char *comment, int utf8)
{
    size_t source_length;
    LPBYTE output;
    if (!comment || !comment[0]) return SPI_ERROR_SUCCESS;
    source_length = strlen(comment);
    if (!utf8) {
        int ret = SpiAllocBuffer(&info->hInfo, &output, source_length + 1u);
        if (ret != SPI_ERROR_SUCCESS) return ret;
        memcpy(output, comment, source_length + 1u);
        SpiUnlockBuffer(&info->hInfo);
        return SPI_ERROR_SUCCESS;
    }
    {
        WCHAR *wide;
        int wide_length;
        int utf8_length;
        size_t output_length;
        if (source_length > INT_MAX) return SPI_ERROR_ALLOCATE_MEMORY;
        wide_length = MultiByteToWideChar(932, 0, comment, (int)source_length,
                                          NULL, 0);
        if (wide_length <= 0) return SPI_ERROR_BROKEN_DATA;
        wide = (WCHAR *)malloc((size_t)wide_length * sizeof(*wide));
        if (!wide) return SPI_ERROR_ALLOCATE_MEMORY;
        if (MultiByteToWideChar(932, 0, comment, (int)source_length,
                                wide, wide_length) != wide_length) {
            free(wide);
            return SPI_ERROR_BROKEN_DATA;
        }
        utf8_length = WideCharToMultiByte(CP_UTF8, 0, wide, wide_length,
                                          NULL, 0, NULL, NULL);
        output_length = 3u + (size_t)utf8_length + 1u;
        if (utf8_length <= 0) {
            free(wide);
            return SPI_ERROR_BROKEN_DATA;
        }
        if (SpiAllocBuffer(&info->hInfo, &output, output_length) !=
            SPI_ERROR_SUCCESS) {
            free(wide);
            return SPI_ERROR_ALLOCATE_MEMORY;
        }
        output[0] = 0xef;
        output[1] = 0xbb;
        output[2] = 0xbf;
        if (WideCharToMultiByte(CP_UTF8, 0, wide, wide_length,
                                (LPSTR)output + 3, utf8_length,
                                NULL, NULL) != utf8_length) {
            free(wide);
            SpiUnlockBuffer(&info->hInfo);
            SpiFreeBuffer(&info->hInfo);
            return SPI_ERROR_BROKEN_DATA;
        }
        output[3 + utf8_length] = 0;
        free(wide);
        SpiUnlockBuffer(&info->hInfo);
    }
    return SPI_ERROR_SUCCESS;
}

/** Decode MAG metadata and return its comment in the requested encoding. */
static int get_image_info(SPI_FILE *fp, PictureInfo *lpInfo, int utf8)
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
    ret = set_comment(lpInfo, image.comment, utf8);
    mag_free(&image);
    return ret;
}

/** Decode MAG metadata and return the original Shift-JIS comment. */
int GetImageInfo(SPI_FILE *fp, PictureInfo *lpInfo)
{
    return get_image_info(fp, lpInfo, 0);
}

/** Decode MAG metadata and return a UTF-8 comment prefixed by a BOM. */
int GetImageInfoW(SPI_FILE *fp, PictureInfo *lpInfo)
{
    return get_image_info(fp, lpInfo, 1);
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
