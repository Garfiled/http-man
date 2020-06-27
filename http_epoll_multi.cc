#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/epoll.h>
#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <utility>
#include <string.h>
#include <thread>
#include <csignal>
#include <sys/syscall.h>
#include <mutex>

#include "log.h"
#include "http-parser.h"

#define PORT 8080
#define MAX_EVENTS 500
#define THREAD_NUM 1

#define ERR_HTTP_NO_KEEPALIVE_CLOSE 2000001
#define ERR_HTTP_READ_EOF           2000002

#define gettid() syscall(SYS_gettid)


class Reactor
{
public:
    std::map<int,Session> session;
};


int acceptConn(int socket_fd,int epoll_fd);
void sendHttpObj(HttpRequest&,std::string);

void signalHandler( int signum )
{
  exit(signum);
}

//g++ http_epoll_multi.cc http-parser.cc -lpthread -std=c++11

int main(int argc, char const *argv[])
{
  int socket_fd,epoll_fd;
  struct sockaddr_in address;
  int addrlen = sizeof(address);

  if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
  {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(PORT);

  if (bind(socket_fd, (struct sockaddr*)&address, sizeof(address))<0)
  {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }
  if (listen(socket_fd, 5) < 0)
  {
    perror("listen");
    exit(EXIT_FAILURE);
  }

  LOGI("listen:%d",PORT);

  std::mutex accept_mutex;
  int acceptor_id = -1;
  for (int i=0;i<5;i++) {
    auto w = new std::thread([socket_fd,i,&accept_mutex,&acceptor_id](){
        int epoll_fd = epoll_create(MAX_EVENTS);
        struct epoll_event event;
        event.events = EPOLLIN|EPOLLET;
        event.data.fd = socket_fd;

        struct epoll_event eventList[MAX_EVENTS];
        Reactor reactor;

        LOGI("start reactor:%d",i);
        while (true) {

          // accept本身没有惊群问题，但是epoll wait会出现
          if (accept_mutex.try_lock()) {
            if (acceptor_id==-1) {
              if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event) < 0) {
                perror("epoll add failed");
                exit(EXIT_FAILURE);
              }
              acceptor_id = i;
            } else {

            }
            accept_mutex.unlock();
          } else {

          }

          //epoll_wait
          int event_count = epoll_wait(epoll_fd, eventList, MAX_EVENTS, 3000);
          if(event_count < 0) {
            LOGE("epoll error: %d",event_count);
            break;
          } else if(event_count == 0) {
            continue;
          }
          for(int i=0; i<event_count; i++) {
            if ((eventList[i].events & EPOLLERR) || (eventList[i].events & EPOLLHUP) || !(eventList[i].events & EPOLLIN)) {
              LOGE("epoll event error");
              close (eventList[i].data.fd);
              continue;
            }
            if (eventList[i].data.fd == socket_fd) {
              int ret = acceptConn(socket_fd,epoll_fd);
              if (ret>0) {
                reactor.session[ret] = Session(ret,128*1024);
              } else {
                LOGE("acceptConn:%d",ret);
              }
            } else {
              if (reactor.session.end() != reactor.session.find(eventList[i].data.fd)) {
                auto sess = reactor.session[eventList[i].data.fd];
                int n = read(sess.fd,sess.buf+sess.len,sess.cap - sess.len);
                int ret = 0;
                if (n<0) {
                  ret = n;
                } else if (n==0) {
                  ret = ERR_HTTP_READ_EOF;
                } else {
                  sess.len += n;
                  ret = processQuery(sess);
                }
                if (ret!=0) {
                  if (ret!=ERR_HTTP_READ_EOF) {
                    LOGE("fd except close: %d %d",eventList[i].data.fd,ret);
                  }
                  close(eventList[i].data.fd);
                  delete(reactor.session[eventList[i].data.fd].buf);
                  reactor.session.erase(eventList[i].data.fd);
                }
              } else {
                LOGE("session not found:%d",eventList[i].data.fd);
                continue;
             }
            }
          }
        }
    });
  }


  signal(SIGINT, signalHandler);
  sleep(300);

  close(epoll_fd);
  close(socket_fd);

  return 0;
}

int handleHttp(HttpRequest& req)
{
  if (req.method=="GET") {
    std::cout << "HttpRequest GET " << req.uri << " " << req.body.size() << std::endl;
  } else if (req.method=="POST") {
    std::cout << "HttpRequest POST " << req.uri << " " << req.body.size() << std::endl;
  } else {
    std::cout << "undefined HttpReq proto" << std::endl;
  }
  sendHttpObj(req,"ok");

  if (req.version == "HTTP/1.0" && req.header["Connection"] != "Keep-Alive") {
    return ERR_HTTP_NO_KEEPALIVE_CLOSE;
  }
  return 0;
}

void sendHttpObj(HttpRequest& req,std::string o)
{
  std::string ret;
  ret.append(req.version);
  ret.append(" 200 OK\r\n",9);
  ret.append("Date: ",6);
  char buf[50];
  time_t now = time(0);
  struct tm tm = *localtime(&now);
  strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S %Z", &tm);
  ret.append(buf,strlen(buf));
  ret.append("\r\n",2);

  ret.append("Content-Length: ",16);
  ret.append(std::to_string(o.size()));
  ret.append("\r\n",2);
  ret.append("Content-Type: text/plain; charset=utf-8\r\n",41);
  if (req.version=="HTTP/1.0" && req.header["Connection"] == "Keep-Alive") {
    ret.append("Connection: keep-alive\r\n\r\n",26);
  } else {
    ret.append("\r\n",2);
  }
  ret.append(o);

  // std::cout << ret << std::endl;
  ::send(req.fd,ret.c_str(),ret.size(),0);
}

int acceptConn(int socket_fd,int epoll_fd)
{
  struct sockaddr_in address;
  socklen_t addrlen = sizeof(struct sockaddr_in);
  bzero(&address, addrlen);

  int client_fd = accept(socket_fd, (struct sockaddr *) &address, &addrlen);

  if (client_fd < 0) {
    return client_fd;
  }
  //将新建立的连接添加到EPOLL的监听中
  struct epoll_event event;
  event.data.fd = client_fd;
  event.events = EPOLLIN;
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event);

//  LOGI("accept newfd: tid:%li cid:%d",gettid(),client_fd);
  return client_fd;
}

