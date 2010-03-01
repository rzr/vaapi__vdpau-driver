/*
 *  ulist.c - List utilities
 *
 *  xvba-video (C) 2009-2010 Splitted-Desktop Systems
 *
 *  @LICENSE@ Proprietary Software
 */

#include "sysdeps.h"
#include "ulist.h"

static UList *list_new(void *data, UList *prev, UList *next)
{
    UList *list = malloc(sizeof(*list));

    if (list) {
        list->data = data;
        list->prev = prev;
        list->next = next;
        if (prev)
            prev->next = list;
        if (next)
            next->prev = list;
    }
    return list;
}

void list_free_1(UList *list)
{
    free(list);
}

void list_free(UList *list)
{
    while (list) {
        UList * const next = list->next;
        list_free_1(list);
        list = next;
    }
}

UList *list_append(UList *list, void *data)
{
    UList * const node = list_new(data, list_last(list), NULL);

    return list ? list : node;
}

UList *list_prepend(UList *list, void *data)
{
    return list_new(data, list ? list->prev : NULL, list);
}

UList *list_first(UList *list)
{
    if (list) {
        while (list->prev)
            list = list->prev;
    }
    return list;
}

UList *list_last(UList *list)
{
    if (list) {
        while (list->next)
            list = list->next;
    }
    return list;
}

unsigned int list_size(UList *list)
{
    unsigned int size = 0;

    while (list) {
        ++size;
        list = list->next;
    }
    return size;
}

UList *list_lookup_full(UList *list, const void *data, UListCompareFunc compare)
{
    if (!list)
        return NULL;

    if (compare) {
        for (; list; list = list->next)
            if (compare(list->data, data))
                return list;
    }
    else {
        for (; list; list = list->next)
            if (list->data == data)
                return list;
    }
    return NULL;
}

#ifdef TEST_LIST
#define UINT_TO_POINTER(i) ((void *)(uintptr_t)(i))
#define POINTER_TO_UINT(p) ((uintptr_t)(void *)(p))

int main(void)
{
    UList *temp, *list = NULL;

    list = list_append(list, UINT_TO_POINTER(1));

    temp = list_append(list, UINT_TO_POINTER(2));
    if (temp != list)
        abort();

    temp = list_append(list, UINT_TO_POINTER(3));
    if (temp != list)
        abort();

    list = list_prepend(list, UINT_TO_POINTER(0));
    if (list == temp)
        abort();

    if (list_size(list) != 4)
        abort();

    if (list_first(list_last(list)) != list)
        abort();

    if (POINTER_TO_UINT(list_last(list)->data) != 3)
        abort();

    if (POINTER_TO_UINT(list_first(temp)->data) != 0)
        abort();

    temp = list_lookup(list, UINT_TO_POINTER(2));
    if (!temp)
        abort();
    if (list_size(temp) != 2)
        abort();

    list_free(list);
    return 0;
}
#endif
