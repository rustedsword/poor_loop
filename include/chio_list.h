/* SPDX-License-Identifier: MIT */
#ifndef CHIO_LIST_H
#define CHIO_LIST_H

#include <stddef.h>

struct chio_list {
	struct chio_list *prev, *next;
};

static inline void chio_list_init(struct chio_list *list)
{
	list->prev = list;
	list->next = list;
}

static inline void chio_list_insert(struct chio_list *at,
				    struct chio_list *entry)
{
	entry->prev = at;
	entry->next = at->next;
	at->next->prev = entry;
	at->next = entry;
}

static inline void chio_list_append(struct chio_list *head,
				    struct chio_list *entry)
{
	chio_list_insert(head->prev, entry);
}

static inline void chio_list_remove(struct chio_list *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	entry->prev = nullptr;
	entry->next = nullptr;
}

static inline bool chio_list_empty(const struct chio_list *list)
{
	return list->next == list;
}

static inline bool chio_list_linked(const struct chio_list *entry)
{
	return entry->next != nullptr;
}

#define chio_container_of(ptr, type, member) \
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))

#define chio_list_for_each(pos, head, member) \
	for (pos = chio_container_of((head)->next, typeof(*pos), member); \
	     &pos->member != (head); \
	     pos = chio_container_of(pos->member.next, typeof(*pos), member))

#define chio_list_for_each_safe(pos, tmp, head, member) \
	for (pos = chio_container_of((head)->next, typeof(*pos), member), \
	     tmp = chio_container_of(pos->member.next, typeof(*pos), member); \
	     &pos->member != (head); \
	     pos = tmp, \
	     tmp = chio_container_of(pos->member.next, typeof(*pos), member))

#endif
