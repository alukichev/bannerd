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
#include <getopt.h>
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
    int frame_num;
    int frame_count;
    unsigned int interval;
};

int Interactive = 0; /* Not daemon */
int LogDebug = 0; /* Do not suppress debug messages when logging */
int RunCount = -1; /* Repeat a given number of times, then exit */
int PreserveMode = 0; /* Do not restore previous framebuffer mode */
char *PipePath = NULL; /* A command pipe to control animation */

static struct screen_info _Fb;

static int usage(char *cmd, char *msg)
{
	char cmd_copy[256];
	char *command;

	strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1);
	command = basename(cmd_copy);

	if (msg)
		printf("%s\n", msg);
	printf("Usage: %s [options] [interval[fps]] frame.bmp ...\n\n",
			command);
	printf("-D, --no-daemon       Do not fork into the background, log\n"
		   "                      to stdout\n");
	printf("-v, --verbose         Do not suppress debug messages in the\n"
		   "                      log (may also be suppressed by syslog"
		                        " configuration)\n");
	printf("-c [<num>],\n"
		   "--run-count[=<num>]   Display the sequence of frames <num>\n"
		   "                      times, then exit. If <num> is incorrect\n"
		   "                      omitted, repeat only once. If it is less\n"
		   "                      than 1, ignore the option\n");
	printf("-p, --preserve-mode   Do not restore framebuffer mode on exit\n"
		   "                      which usually means leaving last frame"
			                    " displayed\n");
	printf("-i <fifo>,\n"
		   "--command-pipe <fifo> Open a named pipe <fifo> and wait for\n"
		   "                      commands. The pipe should exist. If -c is\n"
		   "                      specified, it is ignored. See %s(1) man\n"
		   "                      page for command syntax.\n", command);
	printf("interval              Interval in milliseconds between frames.\n"
		   "                      If \'fps\' suffix is present then it is in\n"
		   "                      frames per second. Default:  41 (24fps)\n");
	printf("frame.bmp ...         list of filenames of frames in BMP"
			                    " format\n");

	return 1;
}

static int get_options(int argc, char **argv)
{
	static struct option _longopts[] = {
			{"no-daemon",	no_argument, 		&Interactive, 1}, /* -D */
			{"verbose",		no_argument, 		&LogDebug, 1},    /* -v */
			{"run-count",	optional_argument,	0, 'c'},          /* -c */
			{"command-pipe",required_argument,	0, 'i'},		  /* -i */
			{"preserve-mode",no_argument,		&PreserveMode, 1},/* -p */
			{0, 0, 0, 0}
	};

	while (1) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "Dvc::i:p", _longopts, &option_index);

		if (c == -1)
			break;
		switch (c) {
		case 0: /* Do nothing, the proper flag was set */
			break;

		case 'D':
			Interactive = 1;
			break;

		case 'v':
			LogDebug = 1;
			break;

		case 'p':
			PreserveMode = 1;
			break;

		case 'c':
			if (!optarg)
				RunCount = 1;
			else {
				int v = (int)strtol(optarg, NULL, 0);

				if (v > 0)
					RunCount = v;
			}
			break;

		case 'i':
			PipePath = optarg;
			break;

		case '?':
			/* The error message has already been printed by getopts_long() */
			return -1;
		}
	}

	return optind;
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

    a->frame_num = 0;
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

	i = get_options(argc, argv);
	if (i < 0)
		return usage(argv[0], NULL);

	for ( ; i < argc; ++i) {
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

/**
 * Run the animation either infinitely or until 'frames' frames have been shown
 */
static int run(struct animation *banner, int frames)
{
	const int infinitely = frames < 0;
	int fnum = banner->frame_num;
	int rc = 0;

	while (infinitely || frames--) {
		int x, y;
		struct image_info *frame = &banner->frames[fnum];

		center2top_left(frame, banner->x, banner->y, &x, &y);
		rc = fb_write_bitmap(&_Fb, x, y, frame);

		if (rc)
			break;

		if (++fnum == banner->frame_count)
			fnum = 0;

		if (banner->interval) {
			const struct timespec sleep_time = {
					.tv_sec = banner->interval / 1000,
					.tv_nsec = (banner->interval % 1000) * 1000000,
			};
			nanosleep(&sleep_time, NULL);
		}
	}

	banner->frame_num = fnum;

	return rc;
}

int interpret_commands(struct animation *banner)
{
	FILE *command_fifo;
	int rc = 0;
	int line_empty = 1;

	LOG(LOG_INFO, "Waiting for commands from \'%s\'", PipePath);
	command_fifo = fopen(PipePath, "r");
	if (command_fifo == NULL)
		ERR_RET(1, "Could not open command pipe");

	while (1) {
		static char _w[] = " \t\r\n";
		char data[255];
		char *token, *line = &data[0];

		if (!fgets(line, sizeof(data), command_fifo)) { /* EOF */
			LOG(LOG_DEBUG, "The other end closed pipe, reopening");
			fclose(command_fifo);
			command_fifo = fopen(PipePath, "r");
			if (command_fifo == NULL)
				ERR_RET(1, "Could not open command pipe");
			continue;
		}

		token = strtok_r(line, _w, &line);
		if (token)
			line_empty = 0;
		else
			continue;

		if (!strncmp(token, "exit", 4)) {
			LOG(LOG_DEBUG, "Exit requested");
			break;
		} else if (!strcmp(token, "run")) {
			int frames;

			token = strtok_r(line, _w, &line);
			if (!token) {
				line_empty = 1;
				continue;
			}

			if (token[0] == ';')
				frames = -1;
			else {
				float factor;
				char *p;

				/* run factor
				 * or
				 * run percent%
				 */
				factor = (float)strtoul(token, &p, 0);
				if (p == token) {
					LOG(LOG_ERR, "Incorrect parameter to \'run\': %s", token);
					rc = 1;
					break;
				}
				if (p[0] == '%')
					factor /= 100;
				else if (p[0] == '.')
					sscanf(token, "%f", &factor);
				/* The rest of the token is ignored */

				if (!factor)
					frames = -1;
				else
					frames = (int)(banner->frame_count * factor);
			}
			LOG(LOG_DEBUG, "run requested for %d frames", frames);
			rc = run(banner, frames);
			if (rc)
				break;
		} else {
			LOG(LOG_ERR, "Unrecognized command \'%s\'", token);
			rc = 1;
			break;
		}
	}

	fclose(command_fifo);

	return rc;
}

int main(int argc, char **argv) {
	struct animation banner = { .interval = (unsigned int)-1, };
	int rc = 0;

	if (init(argc, argv, &banner))
		return 1;
	LOG(LOG_INFO, "started");

	if (PipePath)
		rc = interpret_commands(&banner);
	else
		rc = run(&banner, RunCount * banner.frame_count); /* OK if <0 too */

	return rc;
}

