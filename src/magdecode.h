/**
 * @file magdecode.h
 * @brief Public interface for decoding MAG (MAKI02) image data.
 *
 * The decoder accepts an entire MAG file in memory and returns either indexed
 * pixels or RGB pixels. The interface has no dependency on the Susie API and
 * can therefore also be used by tests and standalone conversion tools.
 */

#ifndef MAGDECODE_H
#define MAGDECODE_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief A decoded MAG image.
 *
 * Indexed images use one byte per pixel in @ref pixels and provide their RGB
 * palette in @ref palette. A 24-bpp image uses three bytes per pixel in RGB
 * order and has no palette entries. Rows are always stored top-down.
 *
 * Instances returned by mag_decode() own @ref pixels and @ref comment and must
 * be released with mag_free().
 */
typedef struct MagImage {
    uint32_t width;             /**< Image width in decoded pixels. */
    uint32_t height;            /**< Image height in decoded pixels. */
    uint16_t origin_x;          /**< Horizontal origin from the MAG header. */
    uint16_t origin_y;          /**< Vertical origin from the MAG header. */
    uint8_t bits_per_pixel;     /**< Output depth: 4, 8, or 24 bits per pixel. */
    uint16_t palette_entries;   /**< Number of valid entries in @ref palette. */
    uint8_t palette[256][3];    /**< Palette entries in RGB component order. */
    uint8_t *pixels;            /**< Top-down indices, or packed RGB at 24 bpp. */
    char *comment;              /**< Optional NUL-terminated MAG comment. */
} MagImage;

/** @brief Result codes returned by mag_decode(). */
enum {
    MAG_OK = 0,             /**< Decoding completed successfully. */
    MAG_ERR_FORMAT = 1,     /**< Signature, header, or compressed data is invalid. */
    MAG_ERR_TRUNCATED = 2,  /**< An input section ends before the required data. */
    MAG_ERR_MEMORY = 3      /**< A memory allocation failed. */
};

/**
 * @brief Test whether a memory buffer contains a supported MAG image.
 *
 * This performs header-level validation only and does not decompress pixels.
 *
 * @param data Pointer to the complete or leading MAG data.
 * @param size Number of readable bytes at @p data.
 * @return Nonzero if the MAG header is supported; otherwise zero.
 */
int mag_probe(const uint8_t *data, size_t size);

/**
 * @brief Decode a complete MAG image from memory.
 *
 * On success, the function initializes @p image and transfers ownership of its
 * allocated buffers to the caller. On failure, @p image is left zero-initialized.
 *
 * @param data Pointer to the complete MAG file data.
 * @param size Size of the MAG file data in bytes.
 * @param image Destination for decoded image properties and pixels.
 * @return A value from the MAG decoder result-code enumeration.
 * @note Call mag_free() for every successfully decoded image.
 */
int mag_decode(const uint8_t *data, size_t size, MagImage *image);

/**
 * @brief Release all buffers owned by a decoded image.
 *
 * The structure is cleared after its buffers are released. Passing `NULL` is
 * permitted.
 *
 * @param image Image to release, or `NULL`.
 */
void mag_free(MagImage *image);

#endif
