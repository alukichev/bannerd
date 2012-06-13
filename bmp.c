/*
 *  Bitmap parsing
 *
 *  Copyright (C) 2012 Alexander Lukichev
 *
 *  Alexander Lukichev <alexander.lukichev@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  version 2 as published by the Free Software Foundation.
 */

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bmp.h"
#include "fb.h"
#include "log.h"

#define BI_RGB          0 /* Bitmap compresion: none */
#define BI_BITFIELDS    3 /* Bitmap compresion: bitfields */

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))
#endif /* ARRAY_SIZE */

#pragma pack(push, 1)

struct bmpfile_header {
    char magic_bytes[2];
    unsigned long filesz;
    unsigned short creator1;
    unsigned short creator2;
    unsigned long bmp_offset;   /* bitmap data starts */
};

struct bitmapinfoheader {
    unsigned long header_size;  /* the size of this header (40 bytes) */
    long width;                 /* bitmap width in pixels (signed) */
    long height;                /* bitmap height in pixels (signed) */
    unsigned short nplanes;     /* number of color planes. Must be 1 */
    unsigned short bpp;         /* number of bits per pixel */
    unsigned long compression;  /* must be 0 */
    unsigned long bmp_size;     /* bitmap size. May be 0 */
    long hres;                  /* horizontal resolution (pixel per meter) */
    long vres;                  /* vertical resolution (pixel per meter) */
    unsigned long ncolors;      /* colors in the color palette, or 0 for 2^n */
    unsigned long nimpcolors;   /* number of important colors, or 0 */
};

struct bitmapinfov3header {
    struct bitmapinfoheader info; /* initial part is the same as infoheader */
    unsigned long red_mask;     /* red channel bit mask */
    unsigned long green_mask;   /* green channel bit mask */
    unsigned long blue_mask;    /* blue channel bit mask */
    unsigned long alpha_mask;   /* alpha channel bit mask */
};

struct bitmapcoreheader {
    unsigned long header_size;  /* the size of this header (12 bytes)*/
    unsigned short width;       /* bitmap width in pixels (unsigned) */
    unsigned short height;      /* bitmap height in pixels (unsigned)*/
    unsigned short nplanes;     /* number of color planes. Must be 1 */
    unsigned short bpp;         /* bits per pixel */
};

typedef union {
    struct bitmapinfoheader info;
    struct bitmapinfov3header infov3;
    struct bitmapcoreheader core;
} DIB_HEADER;

#pragma pack(pop)

typedef void * (*LINE_PARSER)(unsigned long *, void *, int);

static void *_ParseLineARGB4444(unsigned long *out, void *line, int width)
{
    unsigned short *in = (unsigned short *)line;
    int j;

    for (j = 0; j < width; ++j, ++in) {
        unsigned long b, g, r, a;
        unsigned long w = (unsigned long)le16toh(*in);

        /* Simultaneously shift the bitmaps and scale values from 16 to 256 */
        b = (w & 0x000F) << 4;
        g = (w & 0x00F0) << (8 - 4) + 4;
        r = (w & 0x0F00) << (12 - 4) + 4;
        a = (w & 0xF000) << (16 - 4) + 4;
        w = a | r | g | b;

        *out++ = htole32(w);
    }

    return in;
}

static void *_ParseLineRGB4444(unsigned long *out, void *line, int width)
{
    unsigned short *in = (unsigned short *)line;
    int j;

    for (j = 0; j < width; ++j, ++in) {
        unsigned long b, g, r;
        unsigned long w = (unsigned long)le16toh(*in);

        /* Simultaneously shift the bitmaps and scale values from 16 to 256 */
        b = (w & 0x000F) << 4;
        g = (w & 0x00F0) << (8 - 4) + 4;
        r = (w & 0x0F00) << (12 - 4) + 4;
        w = 0xFF000000 | r | g | b;

        *out++ = htole32(w);
    }

    return in;
}

static void *_ParseLineRGB565(unsigned long *out, void *line, int width)
{
    unsigned short *in = (unsigned short *)line;
    int j;

    for (j = 0; j < width; ++j, ++in) {
        unsigned long b, g, r;
        unsigned long w = (unsigned long)le16toh(*in);

        b = (w & 0x001F) >> 0,  b = (b * 0x100) / 0x20, b <<= 0;
        g = (w & 0x07E0) >> 5,  g = (g * 0x100) / 0x40, g <<= 8;
        r = (w & 0xF800) >> 11, r = (r * 0x100) / 0x20, r <<= 16;
        w = 0xFF000000 | r | g | b; /* Alpha channel is 1 */

        *out++ = htole32(w);
    }

    return in;
}

static void *_ParseLineARGB1555(unsigned long *out, void *line, int width)
{
    unsigned short *in = (unsigned short *)line;
    int j;

    for (j = 0; j < width; ++j, ++in) {
        unsigned long b, g, r, a;
        unsigned long w = (unsigned long)le16toh(*in);

        b = (w & 0x001F) >> 0,  b = (b * 0x100) / 0x20, b <<= 0;
        g = (w & 0x03E0) >> 5,  g = (g * 0x100) / 0x20, g <<= 8;
        r = (w & 0x7C00) >> 10, r = (r * 0x100) / 0x20, r <<= 16;
        a = (w & 0x8000) ? 0xFF000000 : 0x00000000;
        w = a | r | g | b;

        *out++ = htole32(w);
    }

    return in;
}

static void *_ParseLineXRGB1555(unsigned long *out, void *line, int width)
{
    unsigned short *in = (unsigned short *)line;
    int j;

    for (j = 0; j < width; ++j, ++in) {
        unsigned long b, g, r;
        unsigned long w = (unsigned long)le16toh(*in);

        b = (w & 0x001F) >> 0,  b = (b * 0x100) / 0x20, b <<= 0;
        g = (w & 0x03E0) >> 5,  g = (g * 0x100) / 0x20, g <<= 8;
        r = (w & 0x7C00) >> 10, r = (r * 0x100) / 0x20, r <<= 16;
        w = 0xFF000000 | r | g | b; /* Alpha channel is 1 */

        *out++ = htole32(w);
    }

    return in;
}

static void *_ParseLineRGB888(unsigned long *out, void *line, int width)
{
    unsigned short *in = (unsigned short *)line;
    int pads = (4 - ((width * 24) / 8) % 4) & 0x3;
    int inc = 0;
    int j;

    for (j = 0; j < width; ++j, inc ^= 1) {
        unsigned long w = (unsigned long)le16toh(*in++);

        if(inc)
            w = (w >> 8) | ((unsigned long)le16toh(*in++) << 8);
        else
            w |= ((unsigned long)le16toh(*in) & 0x00FF) << 16;

        w |= 0xFF000000; /* Alpha channel is 1 */

        *out++ = htole32(w);
    }

    return (unsigned char *)in + pads;
}

static void *_ParseLineRGBA8888(unsigned long *out, void *line, int width)
{
    unsigned long *in = (unsigned long *)line;
    int j;

    for (j = 0; j < width; ++j, ++in) {
        unsigned long w = le32toh(*in);
        unsigned long a = w & 0xFF;

        w = (w >> 8) | (a << 24);

        *out++ = htole32(w);
    }

    return in;
}

static void *_ParseLineARGB8888(unsigned long *out, void *line, int width)
{
    memcpy(out, line, width * 4);

    return ((unsigned long *)line) + width;
}

static void *_ParseLineRGBX8888(unsigned long *out, void *line, int width)
{
    unsigned long *in = (unsigned long *)line;
    int j;

    for (j = 0; j < width; ++j, ++in) {
        unsigned long w = le32toh(*in);
        unsigned long a = w & 0xFF;

        w = 0xFF000000 | (w >> 8); /* Set the alpha channel to 1 */

        *out++ = htole32(w);
    }

    return in;
}

static LINE_PARSER _GetLineParser(DIB_HEADER *dh)
{
    static const struct _parser_pattern {
        LINE_PARSER parser;
        unsigned long red;
        unsigned long green;
        unsigned long blue;
        unsigned long transp;
        unsigned long bpp; /* This field is used if the bitmap has no masks */
    } _mask_parsers[] = {
      {&_ParseLineARGB4444, 0x0F00,    0x00F0,    0x000F,    0xF000,     0},
      {&_ParseLineRGB4444,  0x0F00,    0x00F0,    0x000F,    0x0000,     0},
      {&_ParseLineRGB565,   0xF800,    0x07E0,    0x001F,    0x0000,     0},
      {&_ParseLineARGB1555, 0x7C00,    0x03E0,    0x001F,    0x8000,     0},
      {&_ParseLineXRGB1555, 0x7C00,    0x03E0,    0x001F,    0x0000,    16},
      {&_ParseLineRGB888,   0x00FF,    0xFF00,    0xFF0000,  0x0000,    24},
      {&_ParseLineARGB8888, 0x00FF0000,0x0000FF00,0x000000FF,0xFF000000, 32},
      {&_ParseLineRGBA8888, 0xFF000000,0x00FF0000,0x0000FF00,0x000000FF, 0},
      {&_ParseLineRGBX8888, 0xFF000000,0x00FF0000,0x0000FF00,0x00000000, 0},
    };

    /* Handle COREHEADERs first. Only 16bpp 4.4.4.x.x are supported */
    if (dh->core.header_size == sizeof(dh->core) && dh->info.bpp == 16)
        return &_ParseLineARGB4444;

    /* Handle INFOHEADERs */
    if (dh->info.header_size >= sizeof(dh->info)) {
        /* Try to find a parser for uncompressed bitmap (by bpp) */
        if (dh->info.compression == BI_RGB) {
            unsigned int i;

            for (i = 0; i < ARRAY_SIZE(_mask_parsers); ++i)
                if (dh->info.bpp == _mask_parsers[i].bpp) {
                	LOG(LOG_DEBUG, "Default parser for %ubpp: %u",
                			dh->info.bpp, i);
                    return _mask_parsers[i].parser;
                }
        }

        /* Try to find parser for bitmasked bitmap */
        if (dh->info.compression == BI_BITFIELDS
                && dh->info.header_size >= sizeof(dh->infov3)) {
            unsigned int i;
            struct bitmapinfov3header * h = &dh->infov3;

            LOG(LOG_DEBUG, "%s(): bit masks b = %08lX, g = %08lX, r = %08lX, a = %08lX",
                   __func__, h->blue_mask, h->green_mask,
                   h->red_mask, h->alpha_mask);

            for (i = 0; i < ARRAY_SIZE(_mask_parsers); ++i) {
                const struct _parser_pattern * p = &_mask_parsers[i];

                if (h->red_mask == p->red
                        && h->green_mask == p->green
                        && h->blue_mask == p->blue
                        && h->alpha_mask == p->transp) {
                    LOG(LOG_DEBUG, "%s(): found parser %d", __func__, i);
                    return p->parser;
                }
            }
        }
    }

    return NULL;
}

static int _ParseBitmap(char *from, struct image_info *image,
                        unsigned long in_size, DIB_HEADER *dh)
{
    unsigned long *out;
    int width = (dh->info.height < 0) ? image->width : -image->width;
    int i;
    LINE_PARSER parser;
    char *bitmap_start = from;

    image->pixel_buffer = malloc(image->width * image->height * sizeof(*out));

    if (!image->pixel_buffer)
        return -1;

    out = (dh->info.height < 0)
            ? image->pixel_buffer
            : image->pixel_buffer + (image->height - 1) * abs(width);

    parser = _GetLineParser(dh);
    if (parser == NULL) {
        LOG(LOG_ERR, "Could not find parser for the bitmap");
        return -1;
    }

    for (i = 0; i < image->height; ++i, out += width) {
        from = parser(out, from, abs(width));

        /* A quick and dirty test for incomplete bitmaps in the file */
        /* We have already read outside from */
        if (in_size < from - bitmap_start) {
            LOG(LOG_ERR, "Corrupt BMP, not enough pixels in the file");
            return -1;
        }
    }

    return 0;
}

#if 0
static void _DumpInfoheader(struct bitmapinfoheader * ih)
{
    printf("Header size %lu\nImage %ldx%ldx%hu\nPlanes: %hu\nCompression: %lx\nBitmap size %lu\nResolution: %ldx%ld\nncolors = %lu, nimpcolors = %lu\n",
           ih->header_size, ih->width, ih->height, ih->bpp, ih->nplanes, ih->compression, ih->bmp_size, ih->hres, ih->vres,
           ih->ncolors, ih->nimpcolors);
}
#endif /* 0 */

static int _ParseHeaders(const char *filename, int fd,
                     struct bmpfile_header *bh, DIB_HEADER *dh)
{
    struct stat fst;

    if (stat(filename, &fst))
        ERR_RET(-1, "Could not stat %s", filename);

    /* TODO: convert all the values in headers to host endianness for
     * portability. */

    if (bh->magic_bytes[0] != 'B' || bh->magic_bytes[1] != 'M'
            || bh->filesz != (unsigned long)fst.st_size
            || (unsigned long)fst.st_size <= bh->bmp_offset) {
        LOG(LOG_ERR, "Incorrect bitmap format in %s", filename);
        return -1;
    }

    if (read(fd, dh, sizeof(DIB_HEADER)) != sizeof(DIB_HEADER))
        ERR_RET(-1, "Could not read the DIB header from %s", filename);

    if (dh->info.header_size < sizeof(dh->info)
            && dh->core.header_size != sizeof(dh->core)) {
        LOG(LOG_ERR, "Unsupported BMP format");
        return -1;
    }

    /* At least BITMAPINFOHEADER bytes */
    if (dh->info.header_size >= sizeof(dh->info)) {
        unsigned long bitmap_size;

        bitmap_size = (abs(dh->info.height) * abs(dh->info.width)
                * dh->info.bpp + 7) / 8;

        if (dh->info.nplanes != 1
                || (dh->info.compression != BI_RGB
                    && dh->info.compression != BI_BITFIELDS)
                || dh->info.ncolors
                || fst.st_size - bh->bmp_offset < dh->info.bmp_size
                || dh->info.bmp_size < bitmap_size) {
            LOG(LOG_ERR, "Unsupported BMP format");
#if 0
            _DumpInfoheader( &dh->info );
#endif /* 0 */
            return -1;
        }
    } else { /* BITMAPCOREHEADER */
        unsigned long bitmap_size;

        bitmap_size = (dh->core.width * dh->core.height
                       * dh->core.bpp + 7) / 8;
        if (fst.st_size - bh->bmp_offset < bitmap_size) {
            LOG(LOG_ERR, "Unsupported BMP format");
            return -1;
        }
    }

    return 0;
}

int bmp_read(const char *filename, struct image_info *bitmap)
{
    int fd;
    struct bmpfile_header bmp_header;
    DIB_HEADER dib_header;
    unsigned char *bmp_buffer;
    size_t bitmap_size;
    int r;

    if ((fd = open(filename, O_RDONLY)) < 0)
        ERR_RET(-1, "Could not open file %s", filename);

    if (read(fd, &bmp_header, sizeof(bmp_header)) != sizeof(bmp_header)) {
        ERR("Could not read header from file %s", filename);
        close(fd);
        return -1;
    }

    if (_ParseHeaders(filename, fd, &bmp_header, &dib_header)) {
        close(fd);
        return -1;
    }

    if (dib_header.core.header_size == sizeof(dib_header.core)) {
        bitmap->width = dib_header.core.width;
        bitmap->height = dib_header.core.height;
        bitmap_size = ((dib_header.core.width * dib_header.core.bpp + 31)
                       / 32) * 4;
        bitmap_size *= dib_header.core.height;
    } else {
        bitmap->width = abs(dib_header.info.width);
        bitmap->height = abs(dib_header.info.height);
        bitmap_size = dib_header.info.bmp_size;
    }

    bmp_buffer = malloc(bitmap_size);
    if (!(bmp_buffer)) {
        LOG(LOG_ERR, "Could not allocate %d bytes of memory for the bitmap",
                bitmap_size);
        close(fd);
        return -1;
    }

    r = lseek(fd, bmp_header.bmp_offset, SEEK_SET) == (off_t)-1
            || read(fd, bmp_buffer, bitmap_size) != (ssize_t)bitmap_size;
    close(fd);

    if (r)
        ERR_RET(-1, "Could not read bitmap %s", filename);

    r = _ParseBitmap(bmp_buffer, bitmap, bitmap_size, &dib_header);
    free(bmp_buffer);

#if 1
    if (!r)
        LOG(LOG_DEBUG, "Parsed bitmap %s: %dx%d, bitmap size in BMP %d bytes",
        		filename, bitmap->width, bitmap->height, bitmap_size);
#endif /* 0 */

    return (r) ? -1 : 0;
}
