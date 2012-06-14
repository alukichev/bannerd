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

#ifndef FB_H
#define FB_H

struct screen_info {
    int fd;
    int width;
    int height;
    int bpp; /* bit per pixel */
    void *fb;
    int stride;
    int fb_size;
};

struct image_info {
    int width;
    int height;
    int is_bmp;
    unsigned long *pixel_buffer;
};



int fb_init(struct screen_info *sd);
void fb_close(struct screen_info *sd, int restore_mode);
int fb_write_bitmap(struct screen_info *sd, int x, int y,
		struct image_info *bitmap);
int fb_omap_update_screen(struct screen_info * sd, int x, int y, int w, int h);

#endif /* FB_H */
