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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

#define STACK_SIZE (1024 * 1024)

/* container stack (must be aligned properly for clone) */
static char container_stack[STACK_SIZE];

/* IOCTL */
#define IOCTL_MONITOR_PID _IOW('a', 'a', int)

/* ================= LOGGING ================= */
void log_event(const char *message) {
    FILE *f = fopen("runtime.log", "a");
    if (!f) return;

    fprintf(f, "%s\n", message);
    fclose(f);
}

/* ================= CHILD PROCESS ================= */
int child_func(void *arg) {
    char **argv = (char **)arg;

    printf("[container] started\n");

    /* hostname isolation */
    if (sethostname("container", 10) < 0) {
        perror("sethostname");
        log_event("ERROR: sethostname failed");
    }

    /* mount proc for container visibility */
    mount("proc", "/proc", "proc", 0, NULL);

    /* chroot */
    if (chroot("rootfs-alpha") != 0) {
        perror("chroot failed");
        log_event("ERROR: chroot failed");
        return 1;
    }

    chdir("/");

    /* execute program */
    execvp(argv[0], argv);

    perror("execvp failed");
    log_event("ERROR: execvp failed");

    return 1;
}

/* ================= MAIN ================= */
int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s run <name> <program>\n", argv[0]);
        printf("  %s list\n", argv[0]);
        printf("  %s stop <name>\n", argv[0]);
        return 1;
    }

    /* ================= RUN ================= */
    if (strcmp(argv[1], "run") == 0) {

        if (argc < 4) {
            printf("Usage: %s run <name> <program>\n", argv[0]);
            return 1;
        }

        char *name = argv[2];
        char *program = argv[3];

        printf("[engine] starting container: %s\n", name);

        char *child_args[] = { program, NULL };

        /* clone flags = isolated container */
        int flags =
            CLONE_NEWPID |
            CLONE_NEWUTS |
            CLONE_NEWNS |
            CLONE_NEWIPC |
            SIGCHLD;

        pid_t pid = clone(child_func,
                          container_stack + STACK_SIZE,
                          flags,
                          child_args);

        if (pid < 0) {
            perror("clone failed");
            log_event("ERROR: clone failed");
            return 1;
        }

        printf("[engine] container '%s' PID: %d\n", name, pid);

        /* send PID to kernel monitor */
        int fd = open("/dev/monitor", O_RDWR);
        if (fd >= 0) {
            if (ioctl(fd, IOCTL_MONITOR_PID, &pid) < 0) {
                perror("ioctl failed");
            }
            close(fd);
        }

        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg),
                 "START container=%s pid=%d program=%s",
                 name, pid, program);

        log_event(log_msg);
    }

    /* ================= LIST ================= */
    else if (strcmp(argv[1], "list") == 0) {

        printf("[engine] active containers (heuristic):\n");

        /* better than grep hack: show clone-based processes */
        system("ps -eo pid,cmd | grep -E 'container|rootfs' | grep -v grep");
    }

    /* ================= STOP ================= */
    else if (strcmp(argv[1], "stop") == 0) {

        if (argc < 3) {
            printf("Usage: %s stop <name>\n", argv[0]);
            return 1;
        }

        char cmd[128];
        snprintf(cmd, sizeof(cmd),
                 "pkill -f %s", argv[2]);

        system(cmd);

        printf("[engine] stopped container: %s\n", argv[2]);

        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg),
                 "STOP container=%s", argv[2]);

        log_event(log_msg);
    }

    else {
        printf("Unknown command\n");
    }

    return 0;
}
