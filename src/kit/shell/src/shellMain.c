/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "os.h"
#include "shell.h"
#include "tconfig.h"
#include "tnettest.h"

pthread_t pid;
static tsem_t cancelSem;

void shellQueryInterruptHandler(int32_t signum, void *sigInfo, void *context) {
  tsem_post(&cancelSem);
}

void *cancelHandler(void *arg) {
  setThreadName("cancelHandler");

  while(1) {
    if (tsem_wait(&cancelSem) != 0) {
      taosMsleep(10);
      continue;
    }

#ifdef LINUX
    int64_t rid = atomic_val_compare_exchange_64(&result, result, 0);
    SSqlObj* pSql = taosAcquireRef(tscObjRef, rid);
    taos_stop_query(pSql);
    taosReleaseRef(tscObjRef, rid);
#else
    printf("\nReceive ctrl+c or other signal, quit shell.\n");
    exit(0);
#endif
  }
  return NULL;
}

int checkVersion() {
  if (sizeof(int8_t) != 1) {
    printf("taos int8 size is %d(!= 1)", (int)sizeof(int8_t));
    return 0;
  }
  if (sizeof(int16_t) != 2) {
    printf("taos int16 size is %d(!= 2)", (int)sizeof(int16_t));
    return 0;
  }
  if (sizeof(int32_t) != 4) {
    printf("taos int32 size is %d(!= 4)", (int)sizeof(int32_t));
    return 0;
  }
  if (sizeof(int64_t) != 8) {
    printf("taos int64 size is %d(!= 8)", (int)sizeof(int64_t));
    return 0;
  }
  return 1;
}

// Global configurations
SShellArguments args = {.host = NULL,
#ifndef TD_WINDOWS
  .password = NULL,
#endif
  .user = NULL,
  .database = NULL,
  .timezone = NULL,
  .restful = false,
  .token = NULL,
  .is_raw_time = false,
  .is_use_passwd = false,
  .dump_config = false,
  .file = "\0",
  .dir = "\0",
  .threadNum = 5,
  .commands = NULL,
  .pktLen = 1000,
  .pktNum = 100,
  .pktType = "TCP",
  .netTestRole = NULL};

/*
 * Main function.
 */
int main(int argc, char* argv[]) {
  /*setlocale(LC_ALL, "en_US.UTF-8"); */
#ifdef WINDOWS
  SetConsoleOutputCP(CP_UTF8);
#endif

  if (!checkVersion()) {
    exit(EXIT_FAILURE);
  }

  char* cloud_url = getenv("TDENGINE_CLOUD_URL");
  if (cloud_url != NULL) {
    char* start = strstr(cloud_url, "http://");
    if (start != NULL) {
      cloud_url = start + strlen("http://");
    } else {
      start = strstr(cloud_url, "https://");
      if (start != NULL) {
        cloud_url = start + strlen("https://");
      }
    }
    
    char* tmp = last_strstr(cloud_url, ":");
    if ((tmp == NULL) && ((tmp + 1) != NULL )) {
      fprintf(stderr, "Invalid format in environment variable TDENGINE_CLOUD_URL: %s\n", cloud_url);
      exit(EXIT_FAILURE);
    } else {
      args.port = atoi(tmp + 1);
      tmp[0] = '\0';
      args.host = cloud_url;
    }
  }
  
  char* cloud_token = getenv("TDENGINE_CLOUD_TOKEN");

  if (cloud_token != NULL) {
    args.token = cloud_token;
  }

  shellParseArgument(argc, argv, &args);

  if (args.dump_config) {
    taosInitGlobalCfg();
    taosReadGlobalLogCfg();

    if (!taosReadGlobalCfg()) {
      printf("TDengine read global config failed");
      exit(EXIT_FAILURE);
    }

    taosDumpGlobalCfg();
    exit(0);
  }

  if (args.netTestRole && args.netTestRole[0] != 0) {
    if (taos_init()) {
      printf("Failed to init taos");
      exit(EXIT_FAILURE);
    }
    taosNetTest(args.netTestRole, args.host, args.port, args.pktLen, args.pktNum, args.pktType);
    exit(0);
  }

  if (args.restful) {
    if (convertHostToServAddr()) {
      exit(EXIT_FAILURE);
    }
  }

  /* Initialize the shell */
  shellInit(&args);

  if (tsem_init(&cancelSem, 0, 0) != 0) {
    printf("failed to create cancel semphore\n");
    exit(EXIT_FAILURE);
  }

  pthread_t spid;
  pthread_create(&spid, NULL, cancelHandler, NULL);

  /* Interrupt handler. */
  taosSetSignal(SIGTERM, shellQueryInterruptHandler);
  taosSetSignal(SIGINT, shellQueryInterruptHandler);
  taosSetSignal(SIGHUP, shellQueryInterruptHandler);
  taosSetSignal(SIGABRT, shellQueryInterruptHandler);

  /* Get grant information */
  shellGetGrantInfo(args.con);

  /* Loop to query the input. */
  while (1) {
    pthread_create(&pid, NULL, shellLoopQuery, args.con);
    pthread_join(pid, NULL);
  }
}
