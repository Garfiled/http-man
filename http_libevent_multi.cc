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

#include "http-parser.h"
#include "log.h"

#define ERR_HTTP_NO_KEEPALIVE_CLOSE 2000001
#define ERR_HTTP_READ_EOF           2000002


#define REACTOR_COUNT 5

void accept_conn_cb(struct evconnlistener *listener, evutil_socket_t fd,
                    struct sockaddr *address, int socklen, void *arg);
void sendHttpObj(HttpRequest&,std::string);
void reactor_worker(void* arg);




class Server
{
public:
    Server(int count);
    ~Server();
public:
    std::vector<struct event_base*> reactors;
    int64_t seq;
};

Server::Server(int count)
{
  for (int i=0;i<count;i++) {
    struct event_base *reactor_base = event_base_new();
    this->reactors.push_back(reactor_base);
    auto w = new std::thread(reactor_worker,reactor_base);
  }
}

Server::~Server()
{

}

void timer_cb(int sock, short event, void *arg)
{
}

void reactor_worker(void* arg)
{
  std::cout << "reactor_worker:" << std::endl;
  auto reactor_base = (struct event_base *)arg;

  // 这里是因为base必须要有一个永久事件，不然event_base_dispatch 就退出了
  struct event* evt_timeout = event_new(reactor_base, -1, EV_TIMEOUT | EV_PERSIST, timer_cb, nullptr);

  //设置定时事件
  struct timeval tv = {30, 0};
  event_add(evt_timeout, &tv);

  event_base_dispatch(reactor_base);
  std::cout << "reactor_worker exit:" << std::endl;
}

//g++ http-libevent.cc http-parser.cc -levent -std=c++11
int main()
{
  std::cout <<getpid() << " " << tid << std::endl;
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
  if (listener == nullptr)
  {
    std::cerr << "Couldn't create listener" << std::endl;
    return 1;
  }
  std::cout << "listen:" << port << std::endl;

  event_base_dispatch(base);
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
void accept_conn_cb(struct evconnlistener *listener, evutil_socket_t fd,
                    struct sockaddr *address, int socklen, void *arg)
{

  auto server = (Server*)arg;
  server->seq++;
  auto sess = new Session(fd,128*1024);

  // 设置 socket 为非阻塞
  evutil_make_socket_nonblocking(fd);
//  auto *base = evconnlistener_get_base(listener);
  // 不确定这种方式是否是线程安全的，如果不安全可以改用管道通知的方式
  auto reactor_base = server->reactors[server->seq%server->reactors.size()];
  auto *evt = bufferevent_socket_new(reactor_base, fd, BEV_OPT_CLOSE_ON_FREE);
  bufferevent_setcb(evt, echo_read_cb, nullptr, echo_event_cb, sess);
  bufferevent_enable(evt, EV_READ|EV_WRITE);

}