/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
*/

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

/* ================= DATA STRUCTURES ================= */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ================= BUFFER ================= */

static int bounded_buffer_init(bounded_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
    return 0;
}

int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);

    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);

    if (b->count == 0 && b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ================= LOGGING THREAD ================= */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    mkdir(LOG_DIR, 0777);

    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        FILE *f = fopen(path, "a");
        if (f) {
            fwrite(item.data, 1, item.length, f);
            fwrite("\n", 1, 1, f);
            fclose(f);
        }
    }

    return NULL;
}

/* ================= CHILD ================= */

int child_fn(void *arg)
{
    char **argv = (char **)arg;

    sethostname("container", 10);

    if (chroot("rootfs") != 0) {
        perror("chroot");
    }

    chdir("/");

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    execvp(argv[0], argv);
    perror("exec");
    return 1;
}

/* ================= SOCKET IPC ================= */

static int create_server_socket()
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(fd, 10);
    return fd;
}

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;

    write(fd, req, sizeof(*req));
    close(fd);
    return 0;
}

/* ================= SUPERVISOR ================= */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.server_fd = create_server_socket();
    bounded_buffer_init(&ctx.log_buffer);

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    printf("Supervisor running... rootfs=%s\n", rootfs);

    while (!ctx.should_stop) {
        control_request_t req;
        int client = accept(ctx.server_fd, NULL, NULL);
        if (client < 0) continue;

        read(client, &req, sizeof(req));
        close(client);

        if (req.kind == CMD_STOP) {
            ctx.should_stop = 1;
        }
    }

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    return 0;
}

/* ================= CLI ================= */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req = {0};
    req.kind = CMD_START;

    strncpy(req.container_id, argv[2], sizeof(req.container_id));
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs));
    strncpy(req.command, argv[4], sizeof(req.command));

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req = {0};
    req.kind = CMD_RUN;

    strncpy(req.container_id, argv[2], sizeof(req.container_id));
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs));
    strncpy(req.command, argv[4], sizeof(req.command));

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req = {0};
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req = {0};
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id));
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req = {0};
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id));
    return send_control_request(&req);
}

/* ================= MAIN ================= */

int main(int argc, char *argv[])
{
    if (argc < 2) return 1;

    if (!strcmp(argv[1], "supervisor"))
        return run_supervisor(argv[2]);

    if (!strcmp(argv[1], "start")) return cmd_start(argc, argv);
    if (!strcmp(argv[1], "run")) return cmd_run(argc, argv);
    if (!strcmp(argv[1], "ps")) return cmd_ps();
    if (!strcmp(argv[1], "logs")) return cmd_logs(argc, argv);
    if (!strcmp(argv[1], "stop")) return cmd_stop(argc, argv);

    return 0;
}
