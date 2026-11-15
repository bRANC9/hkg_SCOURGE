/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * mock_dm_shm - create the vendor DMCDEVINFO SysV shared-memory segment.
 *
 * dmengine's dmc_getDeviceInfo() derives a key with ftok() from the fixed
 * qipc path and attaches a 932-byte segment without IPC_CREAT.  This lab
 * helper creates that path and segment, and fills the two fields the reader
 * validates: a VIN string at offset 8 that starts with VIN: and ends with
 * _AVN, and a non-empty software version at offset 0xF0.
 *
 * It is a lab fixture, not the vendor device-info publisher.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DMCD_PATH \
    "/tmp/qipc_sharedmemory_DMCDEVINFO3c645f96a1028473d1fea28590e7f889b293c459"
#define DMCD_KEY_ID 0x51
#define DMCD_SIZE 0x3a4

int main(void)
{
    int fd = open(DMCD_PATH, O_CREAT | O_RDWR, 0600);
    key_t key;
    int id;
    void *memory;
    if (fd < 0) {
        perror("open");
        return 1;
    }
    close(fd);
    key = ftok(DMCD_PATH, DMCD_KEY_ID);
    if (key == (key_t)-1) {
        perror("ftok");
        return 1;
    }
    id = shmget(key, DMCD_SIZE, IPC_CREAT | 0600);
    if (id < 0) {
        perror("shmget");
        return 1;
    }
    memory = shmat(id, NULL, 0);
    if (memory == (void *)-1) {
        perror("shmat");
        return 1;
    }
    memset(memory, 0, DMCD_SIZE);
    strcpy((char *)memory + 8, "VIN:LABVIN0000000000_AVN");
    strcpy((char *)memory + 0xF0, "SWV:LAB-SW-1.0");
    shmdt(memory);
    printf("MOCK_DM_SHM_READY key=%d id=%d size=%d\n",
           (int)key, id, DMCD_SIZE);
    fflush(stdout);
    return 0;
}
