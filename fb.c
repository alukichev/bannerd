/*
 *  A framebuffer animation daemon
 *
 *  Copyright (C) 2012 Alexander Lukichev
 *
 *  Alexander Lukichev <alexander.lukichev@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  version 2 as published by the Free Software Foundation.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <linux/fb.h>
#include <linux/omapfb.h>

#include "fb.h"
#include "log.h"

static struct fb_var_screeninfo old_fb_mode;

int fb_init(struct screen_info *sd)
{
    struct fb_var_screeninfo var_info;
    struct fb_fix_screeninfo fix_info;
    const struct fb_bitfield color = { .length = 8, .offset = 0, .msb_right = 0 };
    int i;
    unsigned long * fb;

    sd->fd = open("/dev/fb0", O_RDWR);

    if (sd->fd < 0)
        ERR_RET(-1, "Unable to open framebuffer");

    if(ioctl(sd->fd, FBIOGET_VSCREENINFO, &var_info)
            || ioctl(sd->fd, FBIOGET_FSCREENINFO, &fix_info))
        ERR_RET(-1, "Unable to get screen information");

    LOG(LOG_DEBUG, "Frame buffer screen size %dx%d, line %d bytes,"
    		" %d bpp, buffer size %d bytes", var_info.xres, var_info.yres,
    		fix_info.line_length, var_info.bits_per_pixel,
    		fix_info.line_length * var_info.yres);
    LOG(LOG_DEBUG, "Offsets: r %d, g %d, b %d, a %d", var_info.red.offset,
    		var_info.green.offset, var_info.blue.offset,
    		var_info.transp.offset);
    memcpy(&old_fb_mode, &var_info, sizeof(old_fb_mode));

    // ARGB32
    var_info.bits_per_pixel = 32;
    var_info.red = color;
    var_info.red.offset = 16;
    var_info.green = color;
    var_info.green.offset = 8;
    var_info.blue = color;
    var_info.blue.offset = 0;
    var_info.transp = color;
    var_info.transp.offset = 24;

    var_info.activate = FB_ACTIVATE_NOW;

    if (ioctl(sd->fd, FBIOPUT_VSCREENINFO, &var_info))
        ERR_RET(-1, "Unable to set screen information");

    if (ioctl(sd->fd, FBIOGET_VSCREENINFO, &var_info)
            || ioctl(sd->fd, FBIOGET_FSCREENINFO, &fix_info))
        ERR_RET(-1, "Unable to get screen information");

    sd->width = var_info.xres;
    sd->height = var_info.yres;
    sd->bpp = var_info.bits_per_pixel;
    sd->stride = fix_info.line_length;
    sd->fb_size = fix_info.line_length * var_info.yres;
    sd->fb = mmap(NULL, sd->fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, sd->fd, 0);

    if (sd->fb == MAP_FAILED) {
        ERR("Unable to map the framebuffer into memory");
        sd->fb = NULL;
        return -1;
    }

    fb = (unsigned long *)sd->fb;

    for (i = 0; i < sd->fb_size / 4; ++i, ++fb)
        *fb = 0xFF000000; /* Reset the background to black, set alpha to 1 */

#if 0
    if (fb_omap_update_screen(sd, 0, 0, sd->width, sd->height))
        return -1;
#endif /* 0 */

    LOG(LOG_DEBUG, "Frame buffer open: screen size %dx%d, line %d bytes, "
            "%d bpp, buffer size %d bytes", sd->width, sd->height,
            sd->stride, sd->bpp, sd->fb_size);
    LOG(LOG_DEBUG, "Offsets: r %d, g %d, b %d, a %d", var_info.red.offset,
    		var_info.green.offset, var_info.blue.offset,
    		var_info.transp.offset);

    return 0;
}

void fb_close(struct screen_info *sd)
{
    int r;

    if (sd->fb != NULL)
        munmap(sd->fb, sd->fb_size);

    /* Try to restore the old mode */
    errno = 0;
    r = ioctl(sd->fd, FBIOPUT_VSCREENINFO, &old_fb_mode);
    LOG(LOG_DEBUG, "restore ioctl() returned %d, "
            "errno = %d (%s)", r, errno, strerror(errno));

    close(sd->fd);
}

int fb_omap_update_screen(struct screen_info *sd, int x, int y, int w, int h)
{
    struct omapfb_update_window fb_win;

    if(sd->fd < 0) {
        LOG(LOG_ERR, "Operation on frame buffer after failed init");
        return -1;
    }

    if(x + w <= 0 || x >= sd->width
            || y + h <= 0 || y >= sd->height) {
        LOG(LOG_ERR, "Unable to update a window outside the screen "
            "(%d, %d, %d, %d)", x, y, w, h);
        return -1;
    }

    if(x < 0) {
        w += x;
        x = 0;
    }

    if(y < 0) {
        h += y;
        y = 0;
    }

    if(x + w > sd->width)
        w = sd->width - x;

    if(y + h > sd->height)
        h = sd->height - y;

    fb_win.x = x;
    fb_win.y = y;
    fb_win.width = w;
    fb_win.height = h;
    fb_win.format = OMAPFB_COLOR_ARGB32;

    if (ioctl(sd->fd, OMAPFB_UPDATE_WINDOW, &fb_win))
        ERR_RET(-1, "Failed to update frame buffer window");

    return 0;
}

int fb_write_bitmap(struct screen_info *sd, int x, int y, struct image_info *bitmap)
{
    unsigned char *line;
    unsigned long *in = bitmap->pixel_buffer;
    unsigned long *out;
    int i;
    int w = bitmap->width, clip_l = 0, clip_r = 0;
    int h = bitmap->height;

    if (x + w <= 0 || x >= sd->width
            || y + h <= 0 || y >= sd->height) {
        LOG(LOG_ERR, "Unable to write a bitmap outside the screen "
            "(%d, %d, %d, %d)", x, y, w, h);
        return -1;
    }

    if (x < 0) {
        clip_l = -x;
        in += clip_l; /* Take out from the first line */
        w += x;
        x = 0;
    }

    if (y < 0) {
        in += (-y) * bitmap->width; /* Take out (-y) lines */
        h += y;
        y = 0;
    }

    if (x + w > sd->width) {
        w = sd->width - x;
        clip_r = bitmap->width - w;
    }

    if (y + h > sd->height)
        h = sd->height - y;

    line = (unsigned char *)sd->fb + y * sd->stride;
    out = ((unsigned long *)line) + x;

    for (i = 0; i < h; ++i, line += sd->stride,
            out = ((unsigned long *)line) + x) {
        memcpy(out, in, w * 4);
        in += w + clip_r + clip_l;
    }

    return 0;
}
