#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <thread>
#include <mutex>
#include <condition_variable>


#include <string>
#include <iostream>


#define PORT 8080
#define SERVER "127.0.0.1"


int main(int argc, char const *argv[])
{
  int sock = 0;
  struct sockaddr_in serv_addr;
  char contentGet[] = "GET / HTTP/1.1\r\n\r\n";

  std::string contentPost("POST / HTTP/1.1\r\n");
  int contentLength = 512*1024;
  contentPost.append("Content-Length: ",16);
  contentPost.append(std::to_string(contentLength));
  contentPost.append("\r\n\r\n",4);
  char* body = new char[contentLength];
  contentPost.append(body,contentLength);


  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
  {
    printf("\n Socket creation error \n");
    return -1;
  }

  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(PORT);

  // Convert IPv4 and IPv6 addresses from text to binary form
  if(inet_pton(AF_INET, SERVER, &serv_addr.sin_addr)<=0)
  {
    printf("\nInvalid address/ Address not supported \n");
    return -1;
  }

  if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
  {
    printf("\nConnection Failed \n");
    return -1;
  }
  printf("connect succ\n");
  struct timeval start;
  struct timeval send_end;
  struct timeval end;
  int total_time = 0;
  int total_send = 0;
  int total_cnt = 10;
  char buffer[1024] = {0};
  int dest = 2048*1024/contentLength;
  std::mutex mtx;
  std::condition_variable cv;
  bool running = false;
  std::thread t([sock,&contentPost,dest,&mtx,&cv,&start,total_cnt,&running,&send_end]() {
      for (int i=0;i<total_cnt;i++) {
        std::unique_lock<std::mutex> lk(mtx);
        if (running) {
        } else {
          cv.wait(lk);
        }
        gettimeofday((timeval*)&start,NULL);

        for (int j=0;j<dest;j++) {
          int ret = send(sock , contentPost.c_str() ,contentPost.size()  , 0 );
          if (ret<=0) {
            std::cout << "send ret:" << ret << std::endl;
            break;
          }
        }
        gettimeofday((timeval*)&send_end,NULL);
        running = false;
      }
  });

  for (int i=0;i<total_cnt;i++)
  {
    running = true;
    cv.notify_one();
    int count = 0;
    while (true) {
      int n = read(sock , buffer, 1024);
      count += n/118;
      if (count >=dest) {
        break;
      }
    }
    gettimeofday(&end,NULL);
    int tuse = end.tv_sec*1000000 + end.tv_usec-start.tv_sec*1000000 - start.tv_usec;
    int tuse_send = send_end.tv_sec*1000000 + send_end.tv_usec-start.tv_sec*1000000 - start.tv_usec;
    printf("timeuse: %d %d us\n",tuse,tuse_send);
    total_time += tuse;
    total_send += tuse_send;
    usleep(650000);
  }
  printf("avg:%d %d us\n",total_time/total_cnt,total_send/total_cnt);
  return 0;
}