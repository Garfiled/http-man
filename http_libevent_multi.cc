#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <arpa/inet.h>
#include <string.h>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <thread>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "http-parser.h"
#include "log.h"

#define ERR_HTTP_NO_KEEPALIVE_CLOSE 2000001
#define ERR_HTTP_READ_EOF           2000002


#define REACTOR_COUNT 5

void accept_conn_cb(struct evconnlistener *listener, evutil_socket_t fd,
                    struct sockaddr *address, int socklen, void *arg);
void reactor_worker(void* arg);
void reactor_notify_cb(evutil_socket_t fd, short event, void *arg);
void echo_read_cb(struct bufferevent *bev, void *arg);
void echo_event_cb(struct bufferevent *bev, short events, void *ctx);

void sendHttpObj(HttpRequest&,std::string);



class Server
{
public:
    Server(int count);
    ~Server();
public:
    std::vector<int> reactor_notify_fds;
    int64_t seq;
};

class Reactor
{
public:
    int id;
    int notify_fd;
    struct event_base* event_base;
};

Server::Server(int count)
{
  for (int i=0;i<count;i++) {
    std::string fifo = "reactor.fifo." + std::to_string(i);
    int ret = mkfifo(fifo.c_str(),0777);
    if (ret!=0) {
      LOGE("mkfifo failed:%d",ret);
      exit(1);
    }
    int notify_fd = open(fifo.c_str(), O_CREAT |O_RDWR | O_NONBLOCK);
    if (notify_fd<=0) {
      LOGE("open fifo failed:%d",notify_fd);
      exit(1);
    }
    auto reactor = new Reactor();
    reactor->id = i;
    reactor->notify_fd = notify_fd;
    reactor->event_base = event_base_new();
    auto w = new std::thread(reactor_worker,reactor);

    this->reactor_notify_fds.push_back(notify_fd);
  }
}

Server::~Server()
{
  for (int i=0;i<reactor_notify_fds.size();i++) {
    close(reactor_notify_fds[i]);
  }

}

//g++ http_libevent_multi.cc http-parser.cc -levent -lpthread -std=c++11
int main()
{
  short port = 8000;
  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = htonl(INADDR_ANY);
  sin.sin_port = htons(port);
  auto base = event_base_new();

  Server server(REACTOR_COUNT);

  auto *listener = evconnlistener_new_bind(
      base, accept_conn_cb, &server,
      LEV_OPT_CLOSE_ON_FREE|LEV_OPT_REUSEABLE, -1,
      reinterpret_cast<struct sockaddr *>(&sin), sizeof(sin));
  if (listener == nullptr) {
    LOGE("Couldn't create listener");
    return 1;
  }

  LOGI("listen:%d",port);
  event_base_dispatch(base);
  return 0;
}

void accept_conn_cb(struct evconnlistener *listener, evutil_socket_t fd,
                    struct sockaddr *address, int socklen, void *arg)
{
  auto server = (Server*)arg;
  server->seq++;

  int notify_fd = server->reactor_notify_fds[server->seq%server->reactor_notify_fds.size()];
  std::string msg = std::to_string(fd);
  int n = write(notify_fd, msg.c_str(), msg.size());
  if (n<=0) {
    LOGE("write notify failed:%d",n);
  }
}

void reactor_worker(void* arg)
{
  auto reactor = (Reactor*)arg;
  LOGI("reactor_worker:%d",reactor->id);

  auto evfifo = event_new(reactor->event_base,reactor->notify_fd , EV_READ|EV_PERSIST, reactor_notify_cb, reactor);
  event_add(evfifo, nullptr);

  event_base_dispatch(reactor->event_base);

  delete reactor;
  LOGI("reactor_worker exit:%d",reactor->id);
}

void reactor_notify_cb(evutil_socket_t fd, short event, void *arg)
{
  char buf[128] = {0};
  int n = read(fd, buf, sizeof(buf));
  if (n<=0) {
    LOGE("reactor_notify_cb: %d",n);
    return;
  }
  int newfd = std::stoi(buf);
  auto reactor = (Reactor*)arg;
  // 设置 socket 为非阻塞
  evutil_make_socket_nonblocking(newfd);
  auto sess = new Session(newfd,128*1024);

  auto *evt = bufferevent_socket_new(reactor->event_base, newfd, BEV_OPT_CLOSE_ON_FREE);
  bufferevent_setcb(evt, echo_read_cb, nullptr, echo_event_cb, sess);
  bufferevent_enable(evt, EV_READ|EV_WRITE);
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

void echo_write_cb(struct bufferevent *bev, void *arg) {
  std::cout << "echo write cb" << std::endl;
}

void echo_read_cb(struct bufferevent *bev, void *arg)
{
  auto sess_ptr = (Session*)arg;
  int n = bufferevent_read(bev, sess_ptr->buf+sess_ptr->len, sess_ptr->cap - sess_ptr->len);
  int ret = 0;
  if (n<0) {
    ret = n;
  } else if (n == 0) {
    ret = ERR_HTTP_READ_EOF;
  } else {
    sess_ptr->len += n;
    ret = processQuery(*sess_ptr);
  }

  if (ret!=0) {
    if (ret != ERR_HTTP_READ_EOF) {
      std::cout << "fd except close:" << sess_ptr->fd << " "<< ret << std::endl;
    }
    close(sess_ptr->fd);
    delete sess_ptr->buf;
    delete sess_ptr;
  }
}

void echo_event_cb(struct bufferevent *bev, short events, void *ctx)
{
  if (events & BEV_EVENT_ERROR)
  {
    int err = EVUTIL_SOCKET_ERROR();
    std::cerr << "Got an error from bufferevent: "
              << evutil_socket_error_to_string(err)
              << std::endl;
  }
  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR))
  {
    bufferevent_free(bev);
  }
}