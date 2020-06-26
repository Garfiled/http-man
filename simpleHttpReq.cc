#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>

#include <string>


#define PORT 8000
#define SERVER "127.0.0.1"


int main(int argc, char const *argv[])
{
  int sock = 0;
  struct sockaddr_in serv_addr;
  char contentGet[] = "GET / HTTP/1.1\r\n\r\n";

  std::string contentPost("POST / HTTP/1.1\r\n");
  int contentLength = 1;
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
  struct timeval end;
  int total_time = 0;
  int total_cnt = 20;
  char buffer[1024] = {0};
  for (int i=0;i<total_cnt;i++)
  {
    gettimeofday(&start,NULL);
    int ret = send(sock , contentPost.c_str() ,contentPost.size()  , 0 );
    if (ret <=0) {
      printf("send failed:%d\n",ret);
    }
    int n = read(sock , buffer, 1024);
    gettimeofday(&end,NULL);
    int tuse = end.tv_sec*1000000 + end.tv_usec-start.tv_sec*1000000 - start.tv_usec;
    printf("%d\n",tuse);
    total_time += tuse;
    usleep(650000);
  }
  printf("avg:%d\n",total_time/total_cnt);
  return 0;
}