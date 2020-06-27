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
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/futures/Future.h>
#include <map>

#include "../log.h"

#define PORT 8080
#define MAX_EVENTS 500
#define THREAD_NUM 5

#define gettid() syscall(SYS_gettid)

class Session
{
public:
    Session(int _fd,int size)
    {
      fd = _fd;
      buf = new char[size];
      cap = size;
      len = 0;
      start = 0;
    }
    Session():fd(0),buf(nullptr),len(0),cap(0),start(0){};
    int fd;
    char* buf;
    int len;
    int cap;
    int start;
};

class Reactor
{
public:
    std::map<int,Session*> session;
};


int acceptConn(int socket_fd,int epoll_fd);
int handle(Session* sess);

void signalHandler( int signum )
{
  exit(signum);
}

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

  folly::CPUThreadPoolExecutor reactor_executor(THREAD_NUM);
  folly::CPUThreadPoolExecutor handle_executor(8);
  folly::CPUThreadPoolExecutor sender_executor(5);

  for (int i=0;i<THREAD_NUM;i++) {
    auto fut = folly::makeSemiFuture().via(&reactor_executor).thenValue([socket_fd,i,&accept_mutex,&acceptor_id,&handle_executor,&sender_executor](folly::Unit){
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
                reactor.session[ret] = new Session(ret,128*1024);
              } else {
                LOGE("acceptConn:%d",ret);
              }
            } else {
              if (reactor.session.end() != reactor.session.find(eventList[i].data.fd)) {
                auto sess = reactor.session[eventList[i].data.fd];
                int n = read(sess->fd,sess->buf+sess->len,sess->cap - sess->len);
                int ret = 0;
                if (n<0) {
                  ret = n;
                } else if (n==0) {
                  ret = -1;
                } else {
                  sess->len += n;
                }
                if (ret!=0) {
                  if (ret!=-1) {
                    LOGE("fd except close: %d %d",eventList[i].data.fd,ret);
                  }
                  close(sess->fd);
                  delete(sess->buf);
                  reactor.session.erase(sess->fd);
                  delete sess;
                }

                auto fu = folly::makeSemiFuture().via(&handle_executor).thenValue([sess](folly::Unit){
                  return handle(sess);
                }).via(&sender_executor).thenValue([sess](int ret) {
                    if (ret==0) {
                      ::send(sess->fd,"ok",2,0);
                    } else {
                      ::send(sess->fd,"err",3,0);
                    }
                });
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

int handle(Session* sess)
{
  LOGI("handle:%d %d",sess->start, sess->len);
  return 0;
}