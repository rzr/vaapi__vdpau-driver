/*
 *  uqueue.c - Queue utilities
 *
 *  xvba-video (C) 2009-2010 Splitted-Desktop Systems
 *
 *  @LICENSE@ Proprietary Software
 */

#include "sysdeps.h"
#include "uqueue.h"
#include "ulist.h"

struct _UQueue {
    UList       *head;
    UList       *tail;
    unsigned int size;
};

UQueue *queue_new(void)
{
    return calloc(1, sizeof(UQueue));
}

void queue_free(UQueue *queue)
{
    if (!queue)
        return;

    list_free(queue->head);
    free(queue);
}

int queue_is_empty(UQueue *queue)
{
    return !queue || queue->size == 0;
}

UQueue *queue_push(UQueue *queue, void *data)
{
    if (!queue)
        return NULL;

    queue->tail = list_append(queue->tail, data);

    if (!queue->head)
        queue->head = queue->tail;

    ++queue->size;
    return queue;
}

void *queue_pop(UQueue *queue)
{
    UList *list;
    void *data;

    if (!queue || !queue->head)
        return NULL;

    list = queue->head;
    data = list->data;
    queue->head = list->next;
    if (--queue->size == 0)
        queue->tail = NULL;
    list_free_1(list);
    return data;
}

#ifdef TEST_QUEUE
#define UINT_TO_POINTER(i) ((void *)(uintptr_t)(i))
#define POINTER_TO_UINT(p) ((uintptr_t)(void *)(p))

int main(void)
{
    UQueue *queue = queue_new();

    if (!queue_push(queue, UINT_TO_POINTER(1)))
        abort();
    if (!queue_push(queue, UINT_TO_POINTER(2)))
        abort();
    if (queue_is_empty(queue))
        abort();

    if (POINTER_TO_UINT(queue_pop(queue)) != 1)
        abort();
    if (POINTER_TO_UINT(queue_pop(queue)) != 2)
        abort();
    if (!queue_is_empty(queue))
        abort();

    if (!queue_push(queue, UINT_TO_POINTER(3)))
        abort();
    if (POINTER_TO_UINT(queue_pop(queue)) != 3)
        abort();

    queue_free(queue);
}
#endif
