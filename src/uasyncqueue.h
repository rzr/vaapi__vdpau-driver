/*
 *  uasyncqueue.h - Asynchronous queue utilities
 *
 *  xvba-video (C) 2009-2010 Splitted-Desktop Systems
 *
 *  @LICENSE@ Proprietary Software
 */

#ifndef UASYNCQUEUE_H
#define UASYNCQUEUE_H

typedef struct _UAsyncQueue UAsyncQueue;

UAsyncQueue *async_queue_new(void)
    attribute_hidden;

void async_queue_free(UAsyncQueue *queue)
    attribute_hidden;

int async_queue_is_empty(UAsyncQueue *queue)
    attribute_hidden;

UAsyncQueue *async_queue_push(UAsyncQueue *queue, void *data)
    attribute_hidden;

void *async_queue_timed_pop(UAsyncQueue *queue, uint64_t end_time)
    attribute_hidden;

#define async_queue_pop(queue) \
    async_queue_timed_pop(queue, 0)

#endif /* UASYNCQUEUE_H */
