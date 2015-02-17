/*
 *
 * honggfuzz - fuzzing routines
 * -----------------------------------------
 *
 * Author:
 * Robert Swiecki <swiecki@google.com>
 * Felix Gröbert <groebert@google.com>
 *
 * Copyright 2010-2015 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "common.h"
#include "fuzz.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "arch.h"
#include "files.h"
#include "log.h"
#include "report.h"
#include "util.h"

static void fuzz_mangleContent(honggfuzz_t * hfuzz, uint8_t * buf, off_t fileSz)
{
    /*
     * Just copy the file if "-r 0"
     */
    uint64_t changesCnt = fileSz * hfuzz->flipRate;

    if (changesCnt == 0ULL) {
        return;
    }

    if (hfuzz->flipMode == 'b') {
        changesCnt *= 8UL;
    }

    changesCnt = util_rndGet(1, changesCnt);

    for (uint64_t x = 0; x < changesCnt; x++) {
        off_t pos = util_rndGet(0, fileSz - 1);

        if (hfuzz->flipMode == 'b') {
            buf[pos] ^= (1 << util_rndGet(0, 7));
        } else {
            buf[pos] = (uint8_t) util_rndGet(0, 255);
        }
    }
}

static void fuzz_getFileName(honggfuzz_t * hfuzz, char *fileName)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    snprintf(fileName, PATH_MAX, ".honggfuzz.%d.%lu.%lu.%lu.%lu.%s",
             (int)getpid(), (unsigned long int)tv.tv_sec,
             (unsigned long int)tv.tv_usec,
             (unsigned long int)util_rndGet(0, 1 << 30),
             (unsigned long int)util_rndGet(0, 1 << 30), hfuzz->fileExtn);

    return;
}

static size_t fuzz_appendOrTrunc(int fd, off_t fileSz)
{
    const int chance_one_in_x = 10;
    if (util_rndGet(1, chance_one_in_x) != 1) {
        return fileSz;
    }

    off_t maxSz = 2 * fileSz;
    if (fileSz > (1024 * 1024 * 20)) {
        maxSz = fileSz + (fileSz / 4);
    }
    if (fileSz > (1024 * 1024 * 100)) {
        maxSz = fileSz;
    }

    off_t newSz = util_rndGet(1, maxSz);
    if (ftruncate(fd, newSz) == -1) {
        LOGMSG_P(l_WARN,
                 "Couldn't truncate file from '%ld' to '%ld' bytes", (long)fileSz, (long)newSz);
        return fileSz;
    }
    return newSz;
}

static bool fuzz_prepareFileDynamically(honggfuzz_t * hfuzz, fuzzer_t * fuzzer)
{
    memcpy(fuzzer->dynamicFile, hfuzz->dynamicFileBest, hfuzz->dynamicFileBestSz);
    fuzzer->dynamicFileSz = hfuzz->dynamicFileBestSz;

    const int chance_one_in_x = 10;
    if (util_rndGet(1, chance_one_in_x) != 1) {
        fuzzer->dynamicFileSz = util_rndGet(fuzzer->dynamicFileSz, sizeof(fuzzer->dynamicFile));
    }

    fuzz_mangleContent(hfuzz, fuzzer->dynamicFile, fuzzer->dynamicFileSz);

    int dstfd = open(fuzzer->fileName, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (dstfd == -1) {
        LOGMSG_P(l_ERROR,
                 "Couldn't create a temporary file '%s' in the current directory",
                 fuzzer->fileName);
        return false;
    }

    if (!files_writeToFd(dstfd, fuzzer->dynamicFile, fuzzer->dynamicFileSz)) {
        close(dstfd);
        return false;
    }

    close(dstfd);
    return true;
}

static bool fuzz_prepareFile(honggfuzz_t * hfuzz, char *fileName, int rnd_index)
{
    off_t fileSz;
    int srcfd;

    uint8_t *buf = files_mapFileToRead(hfuzz->files[rnd_index], &fileSz, &srcfd);
    if (buf == NULL) {
        LOGMSG(l_ERROR, "Couldn't open and map '%s' in R/O mode", hfuzz->files[rnd_index]);
        return false;
    }

    LOGMSG(l_DEBUG, "Mmaped '%s' in R/O mode, size: %d", hfuzz->files[rnd_index], fileSz);

    int dstfd = open(fileName, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (dstfd == -1) {
        LOGMSG_P(l_ERROR,
                 "Couldn't create a temporary file '%s' in the current directory", fileName);
        files_unmapFileCloseFd(buf, fileSz, srcfd);
        return false;
    }

    fuzz_mangleContent(hfuzz, buf, fileSz);

    if (!files_writeToFd(dstfd, buf, fileSz)) {
        files_unmapFileCloseFd(buf, fileSz, srcfd);
        close(dstfd);
        return false;
    }

    files_unmapFileCloseFd(buf, fileSz, srcfd);

    fuzz_appendOrTrunc(dstfd, fileSz);
    close(dstfd);

    return true;
}

static bool fuzz_prepareFileExternally(honggfuzz_t * hfuzz, char *fileName, int rnd_index)
{
    off_t fileSz;
    int srcfd;

    int dstfd = open(fileName, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (dstfd == -1) {
        LOGMSG_P(l_ERROR,
                 "Couldn't create a temporary file '%s' in the current directory", fileName);
        return false;
    }

    LOGMSG(l_DEBUG, "Created '%f' as an input file", fileName);

    if (hfuzz->inputFile) {
        uint8_t *buf = files_mapFileToRead(hfuzz->files[rnd_index], &fileSz, &srcfd);
        if (buf == NULL) {
            LOGMSG(l_ERROR, "Couldn't open and map '%s' in R/O mode", hfuzz->files[rnd_index]);
            close(dstfd);
            return false;
        }

        LOGMSG(l_DEBUG, "Mmaped '%s' in R/O mode, size: %d", hfuzz->files[rnd_index], fileSz);

        bool ret = files_writeToFd(dstfd, buf, fileSz);
        files_unmapFileCloseFd(buf, fileSz, srcfd);

        if (!ret) {
            close(dstfd);
            return false;
        }
    }

    close(dstfd);

    pid_t pid = vfork();
    if (pid == -1) {
        LOGMSG_P(l_ERROR, "Couldn't vfork");
        return false;
    }

    if (!pid) {
#if defined(PR_SET_PDEATHSIG)
        /*
         * Kill it if the main process goes down
         */
        if (prctl(PR_SET_PDEATHSIG, (long)SIGKILL, 0L, 0L, 0L) == -1) {
            LOGMSG_P(l_ERROR, "prctl(PR_SET_PDEATHSIG, SIGKILL) failed");
            return false;
        }
#endif                          /* defined(PR_SET_PDEATHSIG) */
        /*
         * child performs the external file modifications
         */
        execl(hfuzz->externalCommand, hfuzz->externalCommand, fileName, NULL);
        LOGMSG_P(l_FATAL, "Couldn't execute '%s %s'", hfuzz->externalCommand, fileName);
        return false;
    }

    /*
     * parent waits until child is done fuzzing the input file
     */
    int childStatus;
    int flags = 0;
#if defined(__WNOTHREAD)
    flags |= __WNOTHREAD;
#endif                          /* defined(__WNOTHREAD) */
    while (wait4(pid, &childStatus, flags, NULL) != pid) ;
    if (WIFEXITED(childStatus)) {
        LOGMSG(l_DEBUG, "External command exited with status %d", WEXITSTATUS(childStatus));
        return true;
    }
    if (WIFSIGNALED(childStatus)) {
        LOGMSG(l_ERROR, "External command terminated with signal %d", WTERMSIG(childStatus));
        return false;
    }
    LOGMSG(l_FATAL, "External command terminated abnormally, status: %d", childStatus);
    return false;

    abort();                    /* NOTREACHED */
}

static int fuzz_numOfProc(honggfuzz_t * hfuzz)
{
    int i;
    sem_getvalue(hfuzz->sem, &i);
    return hfuzz->threadsMax - i;
}

static void *fuzz_threadNew(void *arg)
{
    honggfuzz_t *hfuzz = (honggfuzz_t *) arg;
    fuzzer_t fuzzer = {
        .pid = 0,
        .timeStarted = time(NULL),
        .pc = 0ULL,
        .backtrace = 0ULL,
        .access = 0ULL,
        .exception = 0,
        .dynamicFileSz = 0,
    };

    int rnd_index = util_rndGet(0, hfuzz->fileCnt - 1);
    strncpy(fuzzer.origFileName, files_basename(hfuzz->files[rnd_index]), PATH_MAX);
    fuzz_getFileName(hfuzz, fuzzer.fileName);

    if (hfuzz->createDynamically) {
        if (!fuzz_prepareFileDynamically(hfuzz, &fuzzer)) {
            exit(EXIT_FAILURE);
        }
    } else if (hfuzz->externalCommand != NULL) {
        if (!fuzz_prepareFileExternally(hfuzz, fuzzer.fileName, rnd_index)) {
            exit(EXIT_FAILURE);
        }
    } else {
        if (!fuzz_prepareFile(hfuzz, fuzzer.fileName, rnd_index)) {
            exit(EXIT_FAILURE);
        }
    }

    fuzzer.pid = fork();
    if (fuzzer.pid == -1) {
        LOGMSG_P(l_FATAL, "Couldn't fork");
        exit(EXIT_FAILURE);
    }

    if (!fuzzer.pid) {
        /*
         * Ok, kill the parent if this fails
         */
        if (!arch_launchChild(hfuzz, fuzzer.fileName)) {
            LOGMSG(l_ERROR, "Error launching child process, killing parent");
            exit(EXIT_FAILURE);
        }
    }

    LOGMSG(l_INFO, "Launched new process, pid: %d, (%d/%d)", fuzzer.pid,
           fuzz_numOfProc(hfuzz), hfuzz->threadsMax);

    arch_reapChild(hfuzz, &fuzzer);
    unlink(fuzzer.fileName);
    report_Report(hfuzz, fuzzer.report);

    sem_post(hfuzz->sem);

    return NULL;
}

static void *fuzz_threadPid(void *arg)
{
    honggfuzz_t *hfuzz = (honggfuzz_t *) arg;
    if (!arch_archInit(hfuzz)) {
        LOGMSG(l_FATAL, "Couldn't prepare parent for fuzzing");
    }

    fuzzer_t fuzzer = {
        .pid = hfuzz->pid,
        .timeStarted = time(NULL),
        .pc = 0ULL,
        .backtrace = 0ULL,
        .access = 0ULL,
        .exception = 0,
        .report = {'\0'}
        ,
        .dynamicFileSz = 0,
    };

    char fileName[] = ".honggfuzz.empty.XXXXXX";
    int fd;
    if ((fd = mkstemp(fileName)) == -1) {
        LOGMSG_P(l_ERROR, "Couldn't create a temporary file");
        return NULL;
    }
    close(fd);

    strncpy(fuzzer.origFileName, "PID_FUZZING", PATH_MAX);
    strncpy(fuzzer.fileName, fileName, PATH_MAX);

    arch_reapChild(hfuzz, &fuzzer);
    unlink(fuzzer.fileName);
    report_Report(hfuzz, fuzzer.report);

    // There's no more hfuzz->pid to analyze. Just exit
    LOGMSG(l_INFO, "PID: %d exited. Exiting", fuzzer.pid);
    exit(EXIT_SUCCESS);

    return NULL;
}

static void fuzz_runThread(honggfuzz_t * hfuzz, void *(*thread) (void *))
{
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, _HF_PTHREAD_STACKSIZE);
    pthread_attr_setguardsize(&attr, (size_t) sysconf(_SC_PAGESIZE));

    pthread_t t;
    if (pthread_create(&t, &attr, thread, (void *)hfuzz) < 0) {
        LOGMSG_P(l_FATAL, "Couldn't create a new thread");
    }

    return;
}

void fuzz_main(honggfuzz_t * hfuzz)
{
    char semName[PATH_MAX];
    snprintf(semName, sizeof(semName), "honggfuzz.%d.%d.%" PRIx64, getpid(),
             (int)time(NULL), util_rndGet(1, 1ULL << 62));

    hfuzz->sem = sem_open(semName, O_CREAT, 0644, hfuzz->threadsMax);
    if (hfuzz->sem == SEM_FAILED) {
        LOGMSG_P(l_FATAL, "sem_open() failed");
    }
    // If we're doing a PID fuzzing, the parent of the PID will be a
    // dedicated thread anyway
    if (hfuzz->pid) {
        fuzz_runThread(hfuzz, fuzz_threadPid);
    } else {
        if (!arch_archInit(hfuzz)) {
            LOGMSG(l_FATAL, "Couldn't prepare parent for fuzzing");
        }
    }

    for (;;) {
        if (sem_wait(hfuzz->sem) == -1) {
            LOGMSG_P(l_FATAL, "sem_wait() failed");
        }

        if (hfuzz->mutationsMax && (hfuzz->mutationsCnt >= hfuzz->mutationsMax)) {
            /*
             * Sleep a bit to let any running fuzzers terminate
             */
            usleep(1.2 * hfuzz->tmOut * 1000000);
            LOGMSG(l_INFO, "Finished fuzzing %ld times.", hfuzz->mutationsMax);
            sem_destroy(hfuzz->sem);
            exit(EXIT_SUCCESS);
        }

        hfuzz->mutationsCnt++;
        fuzz_runThread(hfuzz, fuzz_threadNew);
    }
}
