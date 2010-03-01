/*
 *  uqueue.h - Queue utilities
 *
 *  xvba-video (C) 2009-2010 Splitted-Desktop Systems
 *
 *  @LICENSE@ Proprietary Software
 */

#ifndef UQUEUE_H
#define UQUEUE_H

typedef struct _UQueue UQueue;

UQueue *queue_new(void)
    attribute_hidden;

void queue_free(UQueue *queue)
    attribute_hidden;

int queue_is_empty(UQueue *queue)
    attribute_hidden;

UQueue *queue_push(UQueue *queue, void *data)
    attribute_hidden;

void *queue_pop(UQueue *queue)
    attribute_hidden;

#endif /* UQUEUE_H */
