#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

int fuzzServer(const uint8_t *Data, size_t Size) {
  char *ip = "127.0.0.1";
  int port = 3535;
  struct sockaddr_in server_addr;
  int sockfd;
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = inet_addr(ip);
  connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
  send(sockfd, Data, Size, 0);
  usleep(1500);
  close(sockfd);
  return 1;
}

char *arg_array[] = {"0", "corpus", "-max_len=60000", "-len_control=30", "-use_value_profile=1", "-dict=dict.txt", NULL};

char **args_ptr = &arg_array[0];
int args_size = 6;

void *launchFuzzer2(void *param) {
  LLVMFuzzerRunDriver(&args_size, &args_ptr, &fuzzServer);
}

void launchFuzzer() {
  pthread_t threadID;
  pthread_create(&threadID, NULL, launchFuzzer2, NULL);
}
