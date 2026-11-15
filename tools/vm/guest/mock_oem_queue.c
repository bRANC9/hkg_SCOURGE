/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * mock_oem_queue - lab OEM handler for the dmengine SysV queue.
 *
 * Uses key 0xd54, reads mtype 0x48 events and optionally sends one mtype
 * 0x36 command with int32 code, int64 fields and optional text. It logs event
 * codes/fields only; it does not implement the vendor AppDMClient policy.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <time.h>
#include <unistd.h>

#define QUEUE_KEY 0xd54
#define CMD_TYPE 0x36
#define EVENT_TYPE 0x48
#define DATA_SIZE 1048

struct message {
    long mtype;
    unsigned char data[DATA_SIZE];
};

static volatile sig_atomic_t running = 1;

static void stop(int sig)
{
    (void)sig;
    running = 0;
}

int main(int argc, char **argv)
{
    int send_code = argc > 1 ? atoi(argv[1]) : -1;
    const char *log_path = argc > 2 ? argv[2] : "/tmp/mock-oem.log";
    int seconds = argc > 3 ? atoi(argv[3]) : 20;
    long long field1 = argc > 4 ? atoll(argv[4]) : 0;
    long long field2 = argc > 5 ? atoll(argv[5]) : 0;
    const char *text = argc > 6 ? argv[6] : "";
    int id = msgget(QUEUE_KEY, IPC_CREAT | 0666);
    FILE *log;
    int events = 0;
    time_t deadline;
    if (id < 0) {
        perror("msgget");
        return 1;
    }
    log = fopen(log_path, "a");
    if (log == NULL) {
        perror("log");
        return 1;
    }
    signal(SIGTERM, stop);
    signal(SIGINT, stop);
    if (send_code >= 0) {
        struct message command;
        memset(&command, 0, sizeof(command));
        command.mtype = CMD_TYPE;
        memcpy(command.data, &send_code, sizeof(send_code));
        memcpy(command.data + 8, &field1, sizeof(field1));
        memcpy(command.data + 16, &field2, sizeof(field2));
        strncpy((char *)command.data + 32, text, DATA_SIZE - 32 - 1);
        if (msgsnd(id, &command, DATA_SIZE, 0) < 0) {
            perror("msgsnd");
            fclose(log);
            return 1;
        }
        fprintf(log, "MOCK_OEM_SENT code=%d field1=%lld field2=%lld text=%s\n",
                send_code, field1, field2, text);
        fflush(log);
    }
    printf("MOCK_OEM_READY id=%d send=%d\n", id, send_code);
    fflush(stdout);
    deadline = time(NULL) + (seconds > 0 ? seconds : 20);
    while (running && time(NULL) < deadline) {
        struct message event;
        while (msgrcv(id, &event, DATA_SIZE, EVENT_TYPE, IPC_NOWAIT) > 0) {
            int code = 0;
            long long field1 = 0;
            long long field2 = 0;
            if (DATA_SIZE >= (int)sizeof(code))
                memcpy(&code, event.data, sizeof(code));
            if (DATA_SIZE >= 8 + (int)sizeof(field1))
                memcpy(&field1, event.data + 8, sizeof(field1));
            if (DATA_SIZE >= 16 + (int)sizeof(field2))
                memcpy(&field2, event.data + 16, sizeof(field2));
            fprintf(log, "MOCK_OEM_EVENT code=%d field1=%lld field2=%lld\n",
                    code, field1, field2);
            fflush(log);
            events++;
        }
        usleep(100000);
    }
    fprintf(log, "MOCK_OEM_DONE events=%d\n", events);
    fclose(log);
    printf("MOCK_OEM_DONE events=%d\n", events);
    fflush(stdout);
    return 0;
}
