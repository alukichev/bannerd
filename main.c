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

#include "animation.h"
#include "commands.h"
#include "fb.h"
#include "log.h"
#include "string_list.h"

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
	       "                      times, then exit. If <num> is omitted,\n"
	       "                      repeat only once. If it is less than\n"
	       "                      1, ignore the option\n");
	printf("-p, --preserve-mode   Do not restore framebuffer mode on exit\n"
	       "                      which usually means leaving last frame"
		                    " displayed\n");
	printf("-i <fifo>,\n"
	       "--command-pipe=<fifo> Open a named pipe <fifo> and wait for\n"
	       "                      commands. The pipe should exist. If -c\n"
	       "                      is specified, it is ignored. See %s(1)\n"
	       "                      man page for command syntax.\n",
	       command);
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
			{"no-daemon",	no_argument,&Interactive, 1}, /* -D */
			{"verbose",	no_argument,&LogDebug, 1},    /* -v */
			{"run-count",	optional_argument,0, 'c'},    /* -c */
			{"command-pipe",required_argument,0, 'i'},    /* -i */
			{"preserve-mode",no_argument,&PreserveMode,1},/* -p */
			{0, 0, 0, 0}
	};

	while (1) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "Dvc::i:p", _longopts,
				&option_index);

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
			/* The error message has already been printed
			 * by getopts_long() */
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

static inline int parse_interval(char *param, unsigned int *interval)
{
	char *p;
	unsigned int v = (unsigned int)strtoul(param, &p, 0);
	int is_fps = (p != param) && *p && !strcmp(p, "fps");

	if (!*p || is_fps) {
		if (is_fps) {
			if (!v) { /* 0fps */
				LOG(LOG_WARNING, "0fps argument in cmdline,"
						" changed to 1fps");
				v = 1;
			}
			v = 1000 / v;
		}
		*interval = v;
		return 0;
	}

	return -1;
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
		if (banner->interval == (unsigned int)-1)
			if (!parse_interval(argv[i], &banner->interval))
				continue;

		filenames_tail = string_list_add(&filenames, filenames_tail,
				argv[i]);
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
	if (animation_init(filenames, filenames_count, &_Fb, banner))
		return 1;
	string_list_destroy(filenames);

	if (banner->frame_count == 1)
		banner->interval = (unsigned int)-1; /* Single frame */
	else if (banner->interval == (unsigned int)-1)
		banner->interval = 1000 / 24; /* 24fps */

	if (!Interactive && daemonify())
		ERR_RET(1, "could not create a daemon");

	return 0;
}

int main(int argc, char **argv) {
	struct animation banner = { .interval = (unsigned int)-1, };
	int rc = 0;

	if (init(argc, argv, &banner))
		return 1;
	LOG(LOG_INFO, "started");

	if (PipePath)
		rc = commands_fifo(PipePath, &banner);
	else
		rc = animation_run(&banner, RunCount * banner.frame_count);

	return rc;
}

