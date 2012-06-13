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
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef SRV_NAME
#define SRV_NAME "bannerd"
#endif

#include "log.h"
#include "fb.h"



struct animation {
    int x; /* Center of frames */
    int y; /* Center of frames */
    struct image_info *frames;
    int frame_count;
    unsigned int interval;
};

int Interactive = 0; /* Not daemon */

static struct screen_info _Fb;

static int init_log(void)
{
	if(Interactive)
		return 0;

	openlog(SRV_NAME, LOG_CONS | LOG_NDELAY, LOG_DAEMON);

	return 0;
}

static int daemonify(void)
{
	pid_t pid, sid;

	pid = fork();

	if(pid < 0)
		return -1;

	if(pid > 0)
		_exit(0); /* Parent process exits here */

	umask(0);
	sid = setsid();

	if(sid < 0)
		return -1;

	if(chdir("/") < 0)
		return -1;

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	return 0;
}

static void free_resources(void)
{
	fb_close(&_Fb);
	LOG(LOG_INFO, "exited");
}

static inline void center2top_left(struct image_info *image, int cx, int cy,
		int *top_left_x, int *top_left_y)
{
	*top_left_x = cx - image->width / 2;
	*top_left_y = cy - image->height / 2;
}

static int init_animation(struct screen_info *fb, struct animation *a)
{
    static const char * _path = "/opt/spinner/";
    static const char * _filenames[] = {
        "spinner01B.bmp", "spinner02B.bmp", "spinner03B.bmp",
        "spinner04B.bmp", "spinner05B.bmp", "spinner06B.bmp",
        "spinner07B.bmp", "spinner08B.bmp", "spinner09B.bmp",
        "spinner10B.bmp"
    };
    static const int _frame_count = sizeof(_filenames) / sizeof(_filenames[0]);

    int i;
    struct image_info *frame;
    int screen_w, screen_h;
    char filepath[200];
    const int path_len = strlen(_path);

    if (!fb->fb_size) {
        LOG(LOG_ERR, "Unable to init animation against uninitialized "
                "framebuffer");
        return -1;
    }

    strncpy(&filepath[0], _path, sizeof(filepath));
    a->frame_count = _frame_count;
    a->frames = malloc(_frame_count * sizeof(struct image_info));

    if (a->frames == NULL) {
        LOG(LOG_ERR, "Unable to get %d bytes of memory for animation",
            _frame_count * sizeof(struct image_info));
        return -1;
    }

    for (i = 0; i < _frame_count; ++i) {
        strncat(filepath, _filenames[i], sizeof(filepath) - path_len);

        if (bmp_read(filepath, &a->frames[i]))
            return -1;

        filepath[path_len] = '\0';
    }

    screen_w = fb->width;
    screen_h = fb->height;
    frame = &a->frames[0];
    a->x = screen_w / 2;
    a->y = screen_h / 2;

    return 0;
}

int main(int argc, char **argv) {
	struct animation banner = { .interval = (unsigned int)-1, };
	int i;
	int rc = 0;

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-i")) {
			Interactive = 1;
			continue;
		}

		if (banner.interval == (unsigned int)-1) {
			banner.interval = (unsigned int)strtoul(argv[i], NULL, 0);
			if (banner.interval)
				continue;
		}
	}

	if (banner.interval == (unsigned int)-1)
		banner.interval = 1000 / 24;

	init_log();
	if (fb_init(&_Fb))
		return 1;
	atexit(free_resources);
	if (init_animation(&_Fb, &banner))
		return 1;

	if (!Interactive && daemonify())
		ERR_RET(1, "could not create a daemon");
	LOG(LOG_INFO, "started");

	i = 0;
	while (1) {
		int x, y;
		struct image_info *frame = &banner.frames[i++ % banner.frame_count];

		center2top_left(frame, banner.x, banner.y, &x, &y);
		rc = fb_write_bitmap(&_Fb, x, y, frame);

		if (rc)
			break;

		if (banner.frame_count > 1 && banner.interval) {
			const struct timespec sleep_time = {
					.tv_sec = banner.interval / 1000,
					.tv_nsec = (banner.interval % 1000) * 1000000,
			};
			nanosleep(&sleep_time, NULL);
		}
	}

	return rc;
}

