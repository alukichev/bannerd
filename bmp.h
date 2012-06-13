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

#ifndef BMP_H
#define BMP_H



struct image_info;

int bmp_read(const char *filename, struct image_info *bitmap);

#endif /* BMP_H */
