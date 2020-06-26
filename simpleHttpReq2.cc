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


#define PORT 8000
#define SERVER "127.0.0.1"


int main(int argc, char const *argv[])
{
  int sock = 0;
  struct sockaddr_in serv_addr;
  char contentGet[] = "GET / HTTP/1.1\r\n\r\n";

  std::string contentPost("POST / HTTP/1.1\r\n");
  int contentLength = 64*1024;
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
  struct timeval start_time;
  struct timeval send_end;
  struct timeval end_time;
  int64_t total_time = 0;
  int64_t total_send = 0;
  int64_t total_cnt = 50000;
  char buffer[1024] = {0};
  std::mutex mtx;
  std::condition_variable cv;
  bool running = false;
  std::thread t([sock,&contentPost,&mtx,&cv,&start_time,total_cnt,&running,&send_end]() {
      {
        std::unique_lock<std::mutex> lk(mtx);
        if (running) {
        } else {
          cv.wait(lk);
        }
      }
      gettimeofday((timeval*)&start_time,NULL);
      for (int i=0;i<total_cnt;i++) {
        int ret = send(sock , contentPost.c_str() ,contentPost.size()  , 0 );
        if (ret<=0) {
          std::cout << "send ret:" << ret << std::endl;
          break;
        }
      }
      gettimeofday((timeval*)&send_end,NULL);
      {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk);
      }
  });

  running = true;
  cv.notify_one();

  int cache = 0;
  int count = 0;
  while (true) {
    int n = read(sock , buffer, 1024);
    if (n<=0) {
      std::cout << "read failed:" << n << std::endl;
      exit(1);
    }
    count += (cache+n)/118;
    if (count>=total_cnt) {
      break;
    }
    cache = (cache+n)%118;
  }
  gettimeofday(&end_time,NULL);
  cv.notify_one();
  t.join();
  int64_t tuse = end_time.tv_sec*1000000 + end_time.tv_usec-start_time.tv_sec*1000000 - start_time.tv_usec;
  int64_t tuse_send = send_end.tv_sec*1000000 + send_end.tv_usec-start_time.tv_sec*1000000 - start_time.tv_usec;
  printf("timeuse:%lld ms  %lld ms\n",tuse/1000,tuse_send/1000);

  printf("iops:%lld throughput:%lld kb\n",total_cnt*1000000/tuse,total_cnt*contentLength*1000000/(tuse*1024));
  return 0;
}