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
#include <signal.h>
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

static void sig_handler(int num)
{
	LOG(LOG_INFO, "signal %d caught", num);
	exit(0);
}

static inline int init_proper_exit(void)
{
	struct sigaction action = { .sa_handler = sig_handler, };

	sigemptyset(&action.sa_mask);

	atexit(free_resources);
	if (sigaction(SIGINT, &action, NULL)
			|| sigaction(SIGTERM, &action, NULL))
		ERR_RET(-1, "could not install signal handlers");

	return 0;
}

static int init_animation(struct screen_info *fb, struct animation *a)
{
    static const char * _path = "/opt/spinner/";
    static const char * _filenames[] = {
        "Bird_000.bmp",	"Bird_001.bmp", "Bird_002.bmp", "Bird_003.bmp",
        "Bird_004.bmp",	"Bird_005.bmp", "Bird_006.bmp", "Bird_007.bmp",
        "Bird_008.bmp",	"Bird_009.bmp", "Bird_010.bmp", "Bird_011.bmp",
        "Bird_012.bmp",	"Bird_013.bmp", "Bird_014.bmp", "Bird_015.bmp",
        "Bird_016.bmp",	"Bird_017.bmp", "Bird_018.bmp", "Bird_019.bmp",
        "Bird_020.bmp",	"Bird_021.bmp", "Bird_022.bmp", "Bird_023.bmp",
        "Bird_024.bmp",	"Bird_025.bmp", "Bird_026.bmp", "Bird_027.bmp",
        "Bird_028.bmp",	"Bird_029.bmp", "Bird_030.bmp", "Bird_031.bmp",
        "Bird_032.bmp",	"Bird_033.bmp", "Bird_034.bmp", "Bird_035.bmp",
        "Bird_036.bmp",	"Bird_037.bmp", "Bird_038.bmp", "Bird_039.bmp",
        "Bird_040.bmp",	"Bird_041.bmp", "Bird_042.bmp", "Bird_043.bmp",
        "Bird_044.bmp",	"Bird_045.bmp", "Bird_046.bmp", "Bird_047.bmp",
        "Bird_048.bmp",	"Bird_049.bmp", "Bird_050.bmp", "Bird_051.bmp",
        "Bird_052.bmp",	"Bird_053.bmp", "Bird_054.bmp", "Bird_055.bmp",
        "Bird_056.bmp",	"Bird_057.bmp", "Bird_058.bmp", "Bird_059.bmp",
        "Bird_060.bmp",	"Bird_061.bmp", "Bird_062.bmp", "Bird_063.bmp",
        "Bird_064.bmp",	"Bird_065.bmp", "Bird_066.bmp", "Bird_067.bmp",
        "Bird_068.bmp",	"Bird_069.bmp", "Bird_070.bmp", "Bird_071.bmp",
        "Bird_072.bmp",	"Bird_073.bmp", "Bird_074.bmp", "Bird_075.bmp",
        "Bird_076.bmp",	"Bird_077.bmp", "Bird_078.bmp", "Bird_079.bmp",
        "Bird_080.bmp",	"Bird_081.bmp", "Bird_082.bmp", "Bird_083.bmp",
        "Bird_084.bmp",	"Bird_085.bmp", "Bird_086.bmp", "Bird_087.bmp",
        "Bird_088.bmp",	"Bird_089.bmp", "Bird_090.bmp", "Bird_091.bmp",
        "Bird_092.bmp",	"Bird_093.bmp", "Bird_094.bmp", "Bird_095.bmp",
        "Bird_096.bmp",	"Bird_097.bmp", "Bird_098.bmp", "Bird_099.bmp",
        "Bird_100.bmp"
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

static int init(int argc, char **argv, struct animation *banner)
{
	int i;

	init_log();

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-i")) {
			Interactive = 1;
			continue;
		}

		if (banner->interval == (unsigned int)-1) {
			banner->interval = (unsigned int)strtoul(argv[i], NULL, 0);
			if (banner->interval)
				continue;
		}
	}

	if (banner->interval == (unsigned int)-1)
		banner->interval = 1000 / 24; /* 24fps */

	if (fb_init(&_Fb))
		return 1;
	if (init_proper_exit())
		return 1;
	if (init_animation(&_Fb, banner))
		return 1;

	if (banner->frame_count == 1)
		banner->interval = (unsigned int)-1; /* Sleep more if a single frame */

	if (!Interactive && daemonify())
		ERR_RET(1, "could not create a daemon");

	return 0;
}

static inline void center2top_left(struct image_info *image, int cx, int cy,
		int *top_left_x, int *top_left_y)
{
	*top_left_x = cx - image->width / 2;
	*top_left_y = cy - image->height / 2;
}

int main(int argc, char **argv) {
	struct animation banner = { .interval = (unsigned int)-1, };
	int i;
	int rc = 0;

	if (init(argc, argv, &banner))
		return 1;
	LOG(LOG_INFO, "started");

	i = 0;
	while (1) {
		int x, y;
		struct image_info *frame = &banner.frames[i++ % banner.frame_count];

		center2top_left(frame, banner.x, banner.y, &x, &y);
		rc = fb_write_bitmap(&_Fb, x, y, frame);

		if (rc)
			break;

		if (banner.frame_count == 1 || banner.interval) {
			const struct timespec sleep_time = {
					.tv_sec = banner.interval / 1000,
					.tv_nsec = (banner.interval % 1000) * 1000000,
			};
			nanosleep(&sleep_time, NULL);
		}
	}

	return rc;
}

