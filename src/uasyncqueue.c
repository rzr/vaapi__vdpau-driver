/*
 *  uasyncqueue.c - Asynchronous queue utilities
 *
 *  xvba-video (C) 2009-2010 Splitted-Desktop Systems
 *
 *  @LICENSE@ Proprietary Software
 */

#include "sysdeps.h"
#include "uasyncqueue.h"
#include "uqueue.h"
#include <pthread.h>

struct _UAsyncQueue {
    UQueue             *queue;
    pthread_mutex_t     mutex;
    pthread_cond_t      cond;
    unsigned int        is_waiting;
};

UAsyncQueue *async_queue_new(void)
{
    UAsyncQueue *queue = malloc(sizeof(*queue));

    if (!queue)
        return NULL;

    queue->queue = queue_new();
    if (!queue->queue)
        goto error;

    if (pthread_cond_init(&queue->cond, NULL) != 0)
        goto error;

    pthread_mutex_init(&queue->mutex, NULL);
    queue->is_waiting = 0;
    return queue;

error:
    async_queue_free(queue);
    return NULL;
}

void async_queue_free(UAsyncQueue *queue)
{
    if (!queue)
        return;

    pthread_mutex_unlock(&queue->mutex);
    queue_free(queue->queue);
    free(queue);
}

int async_queue_is_empty(UAsyncQueue *queue)
{
    return queue && queue_is_empty(queue->queue);
}

static UAsyncQueue *async_queue_push_unlocked(UAsyncQueue *queue, void *data)
{
    queue_push(queue->queue, data);
    if (queue->is_waiting)
        pthread_cond_signal(&queue->cond);
    return queue;
}

UAsyncQueue *async_queue_push(UAsyncQueue *queue, void *data)
{
    if (!queue)
        return NULL;

    pthread_mutex_lock(&queue->mutex);
    async_queue_push_unlocked(queue, data);
    pthread_mutex_unlock(&queue->mutex);
    return queue;
}

static void *
async_queue_timed_pop_unlocked(UAsyncQueue *queue, uint64_t end_time)
{
    if (queue_is_empty(queue->queue)) {
        assert(!queue->is_waiting);
        ++queue->is_waiting;
        if (!end_time)
            pthread_cond_wait(&queue->cond, &queue->mutex);
        else {
            struct timespec timeout;
            timeout.tv_sec  = end_time / 1000000;
            timeout.tv_nsec = 1000 * (end_time % 1000000);
            pthread_cond_timedwait(&queue->cond, &queue->mutex, &timeout);
        }
        --queue->is_waiting;
        if (queue_is_empty(queue->queue))
            return NULL;
    }
    return queue_pop(queue->queue);
}

void *async_queue_timed_pop(UAsyncQueue *queue, uint64_t end_time)
{
    void *data;

    if (!queue)
        return NULL;

    pthread_mutex_lock(&queue->mutex);
    data = async_queue_timed_pop_unlocked(queue, end_time);
    pthread_mutex_unlock(&queue->mutex);
    return data;
}

#ifdef TEST_ASYNC_QUEUE
#include <stdarg.h>

#define UINT_TO_POINTER(i) ((void *)(uintptr_t)(i))
#define POINTER_TO_UINT(p) ((uintptr_t)(void *)(p))

enum {
    CMD_QUIT,
    CMD_TIME,
    CMD_ADD_1,
    CMD_ADD_2,
    CMD_ADD_3,
};

typedef enum {
    MSG_INVOKE,
    MSG_REPLY
} MessageType;

#define MSG_ARG(v) ((uint64_t)(v))

typedef struct {
    MessageType  type;
    unsigned int num_args;
    uint64_t     args[1];
} Message;

static Message *msg_new(MessageType type, unsigned int num_args)
{
    Message *msg;

    msg = malloc(sizeof(*msg) + num_args * sizeof(msg->args[0]));
    if (msg) {
        msg->type     = type;
        msg->num_args = num_args;
    }
    return msg;
}

static void msg_free(Message *msg)
{
    free(msg);
}

static int msg_invoke(UAsyncQueue *queue, int cmd, unsigned int num_args, ...)
{
    Message *msg;
    va_list args;
    unsigned int i;

    msg = msg_new(MSG_INVOKE, 1 + num_args);
    if (!msg)
        return 0;

    msg->args[0] = cmd;
    va_start(args, num_args);
    for (i = 0; i < num_args; i++)
        msg->args[1 + i] = va_arg(args, uint64_t);
    va_end(args);

    if (!async_queue_push(queue, msg)) {
        msg_free(msg);
        return 0;
    }
    return 1;
}

static int msg_wait_for_reply(UAsyncQueue *queue)
{
    Message *msg;
    int ret;

    msg = async_queue_pop(queue);
    if (!msg || msg->type != MSG_REPLY)
        return -1;

    ret = msg->args[0];
    free(msg);
    return ret;
}

typedef struct {
    UAsyncQueue *send_queue;
    UAsyncQueue *recv_queue;
} ConsumerThreadArgs;

static void *consumer(void *arg)
{
    ConsumerThreadArgs * const args = arg;
    Message *msg;
    const char *msg_name;
    unsigned int i, ret, stop = 0;
    uint64_t end_time = 0;

    while (!stop) {
        if (end_time) {
            msg = async_queue_timed_pop(args->recv_queue, end_time);
            if (!msg)
                printf("<timed out>\n");
        }
        msg = async_queue_pop(args->recv_queue);
        if (!msg || msg->type != MSG_INVOKE)
            abort();

        switch (msg->args[0]) {
        case CMD_QUIT:  msg_name = "quit"; break;
        case CMD_TIME:  msg_name = "time"; break;
        case CMD_ADD_1: msg_name = "add1"; break;
        case CMD_ADD_2: msg_name = "add2"; break;
        case CMD_ADD_3: msg_name = "add3"; break;
        default:        msg_name = NULL;   break;
        }
        if (!msg_name)
            abort();

        printf("recv %s", msg_name);
        if (msg->num_args > 1) {
            printf(": %d args:", msg->num_args - 1);
            for (i = 1; i < msg->num_args; i++)
                printf(" %d", (int)msg->args[i]);
        }
        printf("\n");

        ret = 0;
        switch (msg->args[0]) {
        case CMD_QUIT:
            stop = 1;
            break;
        case CMD_TIME:
            end_time = msg->args[1];
            break;
        case CMD_ADD_1:
        case CMD_ADD_2:
        case CMD_ADD_3:
            for (i = 1; i < msg->num_args; i++)
                ret += msg->args[i];
            end_time = 0;
            break;
        }
        msg_free(msg);

        msg = msg_new(MSG_REPLY, 1);
        if (!msg)
            abort();
        msg->args[0] = ret;
        if (!async_queue_push(args->send_queue, msg))
            abort();
    }
    return NULL;
}

int main(void)
{
    struct timespec now;
    uint64_t end_time;
    pthread_t consumer_thread;
    ConsumerThreadArgs consumer_args;
    UAsyncQueue *send_queue;
    UAsyncQueue *recv_queue;
    Message *msg;

    send_queue = async_queue_new();
    if (!send_queue)
        abort();

    recv_queue = async_queue_new();
    if (!recv_queue)
        abort();

    consumer_args.send_queue = recv_queue;
    consumer_args.recv_queue = send_queue;
    if (pthread_create(&consumer_thread, NULL, consumer, &consumer_args) != 0)
        abort();

    sleep(1);
    if (!msg_invoke(send_queue, CMD_ADD_1, 1, MSG_ARG(1)))
        abort();
    if (msg_wait_for_reply(recv_queue) != 1)
        abort();

    sleep(1);
    if (!msg_invoke(send_queue, CMD_ADD_2, 2, MSG_ARG(1), MSG_ARG(2)))
        abort();
    if (msg_wait_for_reply(recv_queue) != 3)
        abort();

    clock_gettime(CLOCK_REALTIME, &now);
    end_time = (1 + now.tv_sec) * 1000000 + (now.tv_nsec / 1000);

    sleep(1);
    if (!msg_invoke(send_queue, CMD_TIME, 1, MSG_ARG(end_time)))
        abort();
    if (msg_wait_for_reply(recv_queue) != 0)
        abort();

    sleep(1);
    if (!msg_invoke(send_queue, CMD_ADD_3, 3, MSG_ARG(1), MSG_ARG(2), MSG_ARG(3)))
        abort();
    if (msg_wait_for_reply(recv_queue) != 6)
        abort();

    sleep(1);
    if (!msg_invoke(send_queue, CMD_QUIT, 0))
        abort();
    if (msg_wait_for_reply(recv_queue) != 0)
        abort();

    pthread_join(consumer_thread, NULL);
    async_queue_free(recv_queue);
    async_queue_free(send_queue);
    return 0;
}
#endif
