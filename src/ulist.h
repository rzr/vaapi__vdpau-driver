/*
 *  ulist.h - List utilities
 *
 *  xvba-video (C) 2009-2010 Splitted-Desktop Systems
 *
 *  @LICENSE@ Proprietary Software
 */

#ifndef ULIST_H
#define ULIST_H

typedef struct _UList UList;
struct _UList {
    void       *data;
    UList      *prev;
    UList      *next;
};

typedef int (*UListCompareFunc)(const void *a, const void *b);

void list_free_1(UList *list)
    attribute_hidden;

void list_free(UList *list)
    attribute_hidden;

UList *list_append(UList *list, void *data)
    attribute_hidden;

UList *list_prepend(UList *list, void *data)
    attribute_hidden;

UList *list_first(UList *list)
    attribute_hidden;

UList *list_last(UList *list)
    attribute_hidden;

unsigned int list_size(UList *list)
    attribute_hidden;

UList *list_lookup_full(UList *list, const void *data, UListCompareFunc compare)
    attribute_hidden;

#define list_lookup(list, data) \
    list_lookup_full(list, data, NULL)

#endif /* ULIST_H */
