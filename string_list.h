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

#ifndef STRING_LIST_H
#define STRING_LIST_H

#include <stdlib.h>

#include "log.h"

struct string_list {
	char *s;
	struct string_list *next;
};

static inline struct string_list *string_list_add(struct string_list **head,
		struct string_list *tail, char *s)
{
	struct string_list *new_tail = malloc(sizeof(struct string_list));

	if (new_tail == NULL)
		ERR_RET(NULL, "could not allocate memory");
	new_tail->s = s;
	new_tail->next = NULL;

	if (tail)
		tail->next = new_tail;
	else
		*head = new_tail;

	return new_tail;
}

static inline void string_list_destroy(struct string_list *head)
{
	while (head) {
		struct string_list *new_head = head->next;

		free(head);
		head = new_head;
	}
}

#endif /* STRING_LIST_H */
