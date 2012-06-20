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
#include <libgen.h>
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

#include "fb.h"
#include "log.h"
#include "string_list.h"

struct animation {
    int x; /* Center of frames */
    int y; /* Center of frames */
    struct image_info *frames;
    int frame_count;
    unsigned int interval;
};

int Interactive = 0; /* Not daemon */
int LogDebug = 0; /* Do not suppress debug messages when logging */
int SingleRun = 0; /* Do not repeat the sequence of images, exit instead */
int PreserveMode = 0; /* Do not restore previous framebuffer mode */

static struct screen_info _Fb;

static int usage(char *cmd, char *msg)
{
	char cmd_copy[256];
	char *command;

	strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1);
	command = basename(cmd_copy);

	if (msg)
		printf("%s\n", msg);
	printf("Usage: %s [-i] [-d] [-s] [interval[fps]] frame.bmp ...\n\n",
			command);
	printf("-i                Do not fork into the background and log"
			                " to stdout\n");
	printf("-d                Do not suppress debug messages in the log\n"
		   "                  (may also be suppressed by syslog"
		                    " configuration)\n");
	printf("-s                Display the sequence of frames only once,\n"
		   "                  then exit\n");
	printf("-p                Do not restore framebuffer mode on exit  \n"
		   "                  which usually means leaving last frame"
			                " displayed\n");
	printf("interval          Interval in milliseconds between frames.\n"
		   "                  If \'fps\' suffix is present then it is in\n"
		   "                  frames per second. Default:  41 (24fps)\n");
	printf("frame.bmp ...     list of filenames of frames in BMP format\n");

	return (msg) ? 1 : 0;
}

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
	fb_close(&_Fb, !PreserveMode);
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

static int init_animation(struct string_list *filenames, int filenames_count,
		struct screen_info *fb, struct animation *a)
{
    int i;
    struct image_info *frame;
    int screen_w, screen_h;

    if (!fb->fb_size) {
        LOG(LOG_ERR, "Unable to init animation against uninitialized "
                "framebuffer");
        return -1;
    }

    a->frame_count = filenames_count;
    a->frames = malloc(filenames_count * sizeof(struct image_info));
    if (a->frames == NULL) {
        LOG(LOG_ERR, "Unable to get %d bytes of memory for animation",
            filenames_count * sizeof(struct image_info));
        return -1;
    }

    for (i = 0; i < filenames_count; ++i, filenames = filenames->next)
        if (bmp_read(filenames->s, &a->frames[i]))
            return -1;

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
	struct string_list *filenames = NULL, *filenames_tail = NULL;
	int filenames_count = 0;

	init_log();

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-i")) {
			Interactive = 1;
			continue;
		}
		if (!strcmp(argv[i], "-d")) {
			LogDebug = 1;
			continue;
		}
		if (!strcmp(argv[i], "-s")) {
			SingleRun = 1;
			continue;
		}
		if (!strcmp(argv[i], "-p")) {
			PreserveMode = 1;
			continue;
		}

		if (banner->interval == (unsigned int)-1) {
			char *p;
			unsigned int v = (unsigned int)strtoul(argv[i], &p, 0);
			int is_fps = (p != argv[i]) && *p && !strcmp(p, "fps");

			if (!*p || is_fps) {
				if (is_fps) {
					if (!v) { /* 0fps */
						LOG(LOG_WARNING, "0fps argument in cmdline,"
								" changed to 1fps");
						v = 1;
					}
					v = 1000 / v;
				}
				banner->interval = v;
				continue;
			}
		}

		filenames_tail = string_list_add(&filenames, filenames_tail, argv[i]);
		if (!filenames_tail)
			return 1;
		else
			filenames_count++;
	}

	if (!filenames_count)
		return usage(argv[0], "No filenames specified");

	if (fb_init(&_Fb))
		return 1;
	if (init_proper_exit())
		return 1;
	if (init_animation(filenames, filenames_count, &_Fb, banner))
		return 1;
	string_list_destroy(filenames);

	if (banner->frame_count == 1)
		banner->interval = (unsigned int)-1; /* Sleep more if a single frame */
	else if (banner->interval == (unsigned int)-1)
		banner->interval = 1000 / 24; /* 24fps */

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

		if (SingleRun && i == banner.frame_count)
			exit(0);
		if (banner.interval) {
			const struct timespec sleep_time = {
					.tv_sec = banner.interval / 1000,
					.tv_nsec = (banner.interval % 1000) * 1000000,
			};
			nanosleep(&sleep_time, NULL);
		}
	}

	return rc;
}

