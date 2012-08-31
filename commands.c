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

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "animation.h"
#include "log.h"

#define TTYPE_NOTOKEN		0
#define TTYPE_INT		0x1000
#define TTYPE_FLOAT		0x2000
#define TTYPE_STRING		0x4000
#define TTYPE_MASK		0xf000

#define TOKEN_WHITESPACE	(TTYPE_NOTOKEN	| 0)
#define TOKEN_CMD_DELIMITER	(TTYPE_NOTOKEN	| 1)

#define TOKEN_PERCENT		(TTYPE_INT	| 2)
#define TOKEN_FLOAT		(TTYPE_FLOAT	| 3)
#define TOKEN_INTEGER		(TTYPE_INT	| 4)
#define TOKEN_CHARACTER		(TTYPE_STRING	| 5)
#define TOKEN_FRAME		(TTYPE_INT	| 6)

#define TOKEN_EXIT		(TTYPE_STRING	| 10)
#define TOKEN_RUN		(TTYPE_STRING	| 11)
#define TOKEN_SKIP		(TTYPE_STRING	| 12)

#define TOKEN_BUFFER_SIZE	255

struct commands_data {
	FILE *command_fifo;
	char *fifo_name;
	char *token_buffer;
	int token_cmd_delimiter;
};

static inline int get_symbol(struct commands_data *parser)
{
	FILE *command_fifo = parser->command_fifo;
	int symbol;

	if (!command_fifo) {
		command_fifo = fopen(parser->fifo_name, "r");
		if (command_fifo == NULL)
			ERR_RET(-1, "Could not open command pipe");
		parser->command_fifo = command_fifo;
	}

	while ((symbol = fgetc(command_fifo)) == EOF) {
		LOG(LOG_DEBUG, "the other end closed pipe, reopening");
		fclose(command_fifo);
		command_fifo = fopen(parser->fifo_name, "r");
		if (command_fifo == NULL)
			ERR_RET(1, "Could not open command pipe");
		parser->command_fifo = command_fifo;
	}

	return symbol;
}

static inline int token_check_fit(int type, int buffer_size)
{
	int req_size;

	switch (type & TTYPE_MASK) {
	case TTYPE_INT:
		req_size = sizeof(int);
		break;

	case TTYPE_FLOAT:
		req_size = sizeof(float);
		break;

	case TTYPE_STRING:
		req_size = 2; /* At least one symbol and one '\0' */
		break;

	default:
		req_size = 0;
		break;
	}

	if (buffer_size < req_size) {
		LOG(LOG_DEBUG, "internal error: buffer passed to get_token()"
				" if not enough to hold it (required size %d"
				" but only %d available)",
				req_size, buffer_size);
		return -1;
	}

	return 0;
}

static inline int token_categorize(int type, char symbol)
{
	switch (type) {
	case TOKEN_INTEGER:
		if (isdigit(symbol))
			break;

		if (symbol == 'f')
			type = TOKEN_FRAME;
		else if (symbol == '%')
			type = TOKEN_PERCENT;
		else if (symbol == '.')
			type = TOKEN_FLOAT;
		else
			type = TTYPE_STRING;
		break;

	case TOKEN_FLOAT:
		if (!isdigit(symbol))
			type = TTYPE_STRING;
		break;

	case TOKEN_CHARACTER:
	case TOKEN_FRAME:
	case TOKEN_PERCENT:
		type = TTYPE_STRING;
		break;

	case TTYPE_STRING:
		break;

	case -1:
		if (isdigit(symbol))
			type = TOKEN_INTEGER;
		else if (symbol == '.') /* .num is float */
			type = TOKEN_FLOAT;
		else if (isalpha(symbol))
			type = TTYPE_STRING;
		else
			type = TOKEN_CHARACTER;
	}
	LOG(LOG_DEBUG, "token categorized to %x due to symbol %c",
			type, symbol);

	return type;
}

static inline int token_convert(char *buffer, int type, void *token, int tsize)
{
	/* We already checked that it fits (except for
	 * string) */
	switch (type) {
	case TOKEN_INTEGER:
	case TOKEN_PERCENT:
	case TOKEN_FRAME:
		*(int *)token = strtoul(buffer, NULL, 0);
		break;

	case TOKEN_FLOAT:
		sscanf(buffer, "%f", token);
		break;

	default:
		if (!(type & TTYPE_STRING)) {
			LOG(LOG_DEBUG, "parser missed a token type, type = %x",
					type);
			break;
		}

		/* The string is properly null-terminated */
		strncpy(token, buffer, tsize);

		if (!strcmp(buffer, "exit"))
			type = TOKEN_EXIT;
		else if (!strcmp(buffer, "run"))
			type = TOKEN_RUN;
		else if (!strcmp(buffer, "skip"))
			type = TOKEN_SKIP;
	}

	LOG(LOG_DEBUG, "token recognized as %x", type);

	return type;
}

static int get_token(struct commands_data *parser, void *token, int token_size)
{
	static char _cmd_delimiters[] = ";\r\n";
	int i;
	int type = -1;
	char *buffer = parser->token_buffer;

	/* Command delimiter was consumed but unreported earlier */
	if (parser->token_cmd_delimiter) {
		parser->token_cmd_delimiter = 0;
		return TOKEN_CMD_DELIMITER;
	}

	/* Get symbols until one of the delimiters or the size limit is reached.
	 * Categorize if not already done so, and act depending on token category
	 * (parse either a word, an integer, a float or whatnot).
	 */
	for (i = 0; i < TOKEN_BUFFER_SIZE - 1; ) {
		char symbol;
		int r = get_symbol(parser);
		int is_delimiter = 0;

		if (r == -1)
			break;
		symbol = (char)r;

		if (isblank(symbol)) {
			if (type == -1) /* Ignore whitespaces before token */
				continue;
		} else {
			is_delimiter = strchr(_cmd_delimiters, symbol) != NULL;
			if (type == -1 && is_delimiter)
				continue; /* Ignore delimiters before token */
		}

		if (isblank(symbol) || is_delimiter) {
			/* The token is ready */
			buffer[i] = '\0';

			if (is_delimiter) /* Report delimiter separately */
				parser->token_cmd_delimiter = 1;

			/* Convert the string into token, based on category */
			type = token_convert(buffer, type, token, token_size);
			break;
		}

		if (isalpha(symbol))
			symbol = tolower(symbol);
		buffer[i] = symbol;
		type = token_categorize(type, symbol);
		if (token_check_fit(type, token_size)) {
			type = -1;
			break;
		}

		i++;
	}

	return type;
}

static inline const char *spell_token_type(int type)
{
	switch (type) {
	case TOKEN_WHITESPACE:
		return "whitespace";
	case TOKEN_CMD_DELIMITER:
		return "command delimiter";
	case TOKEN_PERCENT:
	case TOKEN_FLOAT:
	case TOKEN_INTEGER:
	case TOKEN_FRAME:
		return "number";
	case TOKEN_CHARACTER:
		return "character";
	case TOKEN_EXIT:
	case TOKEN_RUN:
	case TOKEN_SKIP:
		return "command";
	case TTYPE_STRING:
		return "arbitrary character sequence";
	default:
		return "unknown (probably bug)";
	}
}

/*
 * Command syntax: {run OR skip} [duration]
 * duration is: integer% OR float OR {integer}f
 * The latter form ({integer}f) is the pause frame number
 */
static inline int parse_run_skip(int skip, struct animation *banner,
		int *need_exit)
{
	int token_type;
	union {
		float factor;
		int number;
	} token;
	int frames = -1;
	const char *cmd_name = (skip) ? "skip" : "run";

	token_type = get_token(banner->commands, &token,
			sizeof(token));

	switch (token_type) {
	case TOKEN_PERCENT:
		frames = (banner->frame_count * token.number) / 100;
		break;

	case TOKEN_INTEGER:
		frames = banner->frame_count * token.number;
		break;

	case TOKEN_FLOAT:
		frames = (int)(banner->frame_count * token.factor);
		break;

	case TOKEN_FRAME:
		if (token.number < banner->frame_num)
			token.number += banner->frame_count;
		frames = token.number - banner->frame_num;
		break;

	case TOKEN_CMD_DELIMITER:
		break;

	default:
		LOG(LOG_ERR, "incorrect parameter to \'%s\': %s (%x)",
				cmd_name, spell_token_type(token_type),
				token_type);
		*need_exit = 1;
		return -1;
	}

	if (token_type != TOKEN_CMD_DELIMITER) {
		token_type = get_token(banner->commands, &token,
				sizeof(token));
		if (token_type != TOKEN_CMD_DELIMITER) {
			LOG(LOG_ERR, "unexpected remainder of \'%s\': %s",
				cmd_name, spell_token_type(token_type));
			*need_exit = 1;
			return -1;
		}
	}

	LOG(LOG_DEBUG, "%s requested for %d frames", cmd_name, frames);
	if (!skip)
		return animation_run(banner, frames);
	else {
		banner->frame_num = (banner->frame_num + frames)
				% banner->frame_count;
		return 0;
	}
}

static int parse_loop(struct animation *banner)
{
	char command[255];
	char rc = 0;
	int need_exit = 0;

	while (!need_exit) {
		int token_type = get_token(banner->commands,  &command[0],
				sizeof(command));

		switch (token_type) {
		case TOKEN_EXIT:
			LOG(LOG_DEBUG, "exit requested");
			need_exit = 1;
			break;

		case TOKEN_RUN:
			rc = parse_run_skip(0, banner, &need_exit);
			break;

		case TOKEN_SKIP:
			rc = parse_run_skip(1, banner, &need_exit);
			break;

		default:
			if (token_type == TTYPE_STRING)
				LOG(LOG_ERR, "unrecognized command \'%s\'",
						command);
			else
				LOG(LOG_ERR, "unrecognized token or error"
						" while getting it");
			rc = 1;
			need_exit = 1;
			break;
		}
	}

	return rc;
}

int commands_fifo(char *name, struct animation *banner)
{
	int rc;
	struct commands_data *parser = malloc(sizeof(struct commands_data));

	if (!parser)
		ERR_RET(-1, "could not allocate memory");

	parser->fifo_name = name;
	parser->command_fifo = 0;
	parser->token_buffer = malloc(TOKEN_BUFFER_SIZE);
	parser->token_cmd_delimiter = 0;
	banner->commands = parser;

	LOG(LOG_INFO, "Waiting for commands from \'%s\'", name);
	rc = parse_loop(banner);

	if (parser->command_fifo)
		fclose(parser->command_fifo);
	free(parser->token_buffer);
	free(parser);

	return rc;
}

