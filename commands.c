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

#include <stdio.h>
#include <string.h>

#include "animation.h"
#include "log.h"

int commands_fifo(char *name, struct animation *banner)
{
	FILE *command_fifo;
	int rc = 0;
	int line_empty = 1;

	LOG(LOG_INFO, "Waiting for commands from \'%s\'", name);
	command_fifo = fopen(name, "r");
	if (command_fifo == NULL)
		ERR_RET(1, "Could not open command pipe");

	while (1) {
		static char _w[] = " \t\r\n";
		char data[255];
		char *token, *line = &data[0];

		if (!fgets(line, sizeof(data), command_fifo)) { /* EOF */
			LOG(LOG_DEBUG, "The other end closed pipe, reopening");
			fclose(command_fifo);
			command_fifo = fopen(name, "r");
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
			rc = animation_run(banner, frames);
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

