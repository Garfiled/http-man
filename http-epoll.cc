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

#include "http-parser.h"

#define PORT 8080
#define MAX_EVENTS 500
#define THREAD_NUM 1


#define ERR_HTTP_NO_KEEPALIVE_CLOSE 2000001
#define ERR_HTTP_READ_EOF           2000002

class HttpServer
{
public:
    std::map<int,Session> session;
};


int acceptConn(int socket_fd,int epoll_fd);
void sendHttpObj(HttpRequest&,std::string);

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
   
    std::cout << "listen:" << PORT << std::endl; 
    epoll_fd = epoll_create(MAX_EVENTS);
    struct epoll_event event;
    struct epoll_event eventList[MAX_EVENTS];
    event.events = EPOLLIN|EPOLLET;
    event.data.fd = socket_fd;

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event) < 0) {
        perror("epoll add failed");
        exit(EXIT_FAILURE);
    }
    HttpServer server;
    while(1)
    {
        //epoll_wait
        int event_count = epoll_wait(epoll_fd, eventList, MAX_EVENTS, 3000);
        if(event_count < 0) {
            std::cout << "epoll error " << event_count << std::endl;
            break;
        } else if(event_count == 0) {
            continue;
        }
        for(int i=0; i<event_count; i++) {
            if ((eventList[i].events & EPOLLERR) || (eventList[i].events & EPOLLHUP) || !(eventList[i].events & EPOLLIN)) {
                std::cout << "epoll event error" << std::endl;
                close (eventList[i].data.fd);
                continue;
            }
            if (eventList[i].data.fd == socket_fd) {
                int ret = acceptConn(socket_fd,epoll_fd);
                if (ret>0) {
                    server.session[ret] = Session(ret,128*1024);
                }
            } else{
                if (server.session.end() == server.session.find(eventList[i].data.fd)) {
                    std::cout << "session not found " << eventList[i].data.fd << std::endl;
                    continue;
                }
                auto sess = server.session[eventList[i].data.fd];
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
											std::cout << "fd except close:" << eventList[i].data.fd<< " "<< ret << std::endl;
										}
                    close(eventList[i].data.fd);
                    delete(server.session[eventList[i].data.fd].buf);
                    server.session.erase(eventList[i].data.fd);
                }
            }
        }
    }

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
        std::cout << "accept error " << client_fd << std::endl;
        return client_fd;
    }
    //将新建立的连接添加到EPOLL的监听中
    struct epoll_event event;
    event.data.fd = client_fd;
    event.events = EPOLLIN;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event);

    return client_fd;
}

