#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <sys/epoll.h>

#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <queue>
#include <utility>

#define PORT 8080
#define MAX_EVENTS 500
#define THREAD_NUM 1

#define CONTENT_LEN_LIMIT 500*1024*1024

#define ERR_HTTP_NOT_COMPLETE 100001
#define ERR_HTTP_REQ_METHOD   100002
#define ERR_HTTP_REQ_URI      100003
#define ERR_HTTP_REQ_VERSION  100004
#define ERR_HTTP_HEADER_KV    100005
#define ERR_HTTP_HEADER_CONTENT_LENGTH    100006
#define ERR_HTTP_CONTENT_LIMIT            100007

class Session
{
public:
    Session(int client_fd,int size)
    {
        fd = client_fd;
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

class HttpServer
{
public:
    std::map<int,Session> session;
};

class HttpRequest
{
public:
    std::string method;
    std::string uri;
    std::string version;
    std::map<std::string,std::string> header;
    std::string body;

    int fd;
};

void* worker(void* argv);
int acceptConn(int socket_fd,int epoll_fd);
int processQuery(Session&);

// 解析http协议
int parseHttp(char* buf,int* start_p,int length,HttpRequest& req);
int findHeaderEnd(char* buf,int start,int end);
int findLine(char* buf,int start,int end);
int findSpace(char* buf,int start,int end);
int findSub(char* buf,int start,int end);

int myAtoi(char* p,int end,int* val);

void sendHttpOK(int fd);
void handleHttp(HttpRequest& req);

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

    epoll_fd = epoll_create(MAX_EVENTS);
    struct epoll_event event;
    struct epoll_event eventList[MAX_EVENTS];
    event.events = EPOLLIN|EPOLLET;
    event.data.fd = socket_fd;

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event) < 0)
    {
        perror("epoll add failed");
        exit(EXIT_FAILURE);
    }

    // 创建线程池
    pthread_t pids[THREAD_NUM];
    for (int i=0;i<THREAD_NUM;i++)
    {
        int ret = pthread_create(&pids[i],NULL,worker,NULL);
        if (ret!=0)
        {
            perror("create thread failed");
            exit(EXIT_FAILURE);
        }
    }

    HttpServer server;

    //epoll
    while(1)
    {
        //epoll_wait
        int ret = epoll_wait(epoll_fd, eventList, MAX_EVENTS, 3000);

        if(ret < 0)
        {
            std::cout << "epoll error " << ret << std::endl;
            break;
        } else if(ret == 0)
        {
            continue;
        }

        for(int i=0; i<ret; i++)
        {
            if ((eventList[i].events & EPOLLERR) || (eventList[i].events & EPOLLHUP) || !(eventList[i].events & EPOLLIN))
            {
                std::cout << "epoll event error" << std::endl;
                close (eventList[i].data.fd);
                continue;
            }

            if (eventList[i].data.fd == socket_fd)
            {
                int ret = acceptConn(socket_fd,epoll_fd);
                if (ret>0)
                {
                    server.session[ret] = Session(ret,4096);
                }
            }else{
                if (server.session.end() == server.session.find(eventList[i].data.fd))
                {
                    std::cout << "session not found " << eventList[i].data.fd << std::endl;
                    continue;
                }

                int ret = processQuery(server.session[eventList[i].data.fd]);
                if (ret!=0)
                {
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

int processQuery(Session& sess)
{
    int n = read(sess.fd,sess.buf+sess.len,sess.cap - sess.len);
    if (n==0)
    {
        return -1;
    }
    sess.len += n;

    std::cout  << sess.buf << std::endl;
    // 开始解析命令，并决定命令到worker线程的路由

    int start=0;
    HttpRequest req;
    req.fd = sess.fd;
    int ret = parseHttp(sess.buf,&start,sess.len,req);
    std::cout <<"debug " << ret << " " << sess.len << " " << n << " " << sess.cap << " " << start << std::endl;
    if (ret == 0)
    {
        sess.start = start;
        if (start >= sess.len)
        {
            sess.len = 0;
            sess.start = 0;
        }

        handleHttp(req);

    } else if (ret==ERR_HTTP_NOT_COMPLETE)
    {
        if (sess.len>=sess.cap)
        {
            if (sess.len<CONTENT_LEN_LIMIT)
            {
                char* newBuf = new char[2*sess.cap];
                memcpy(newBuf,sess.buf+sess.start,sess.cap-sess.start);
                delete sess.buf;
                sess.buf = newBuf;
                sess.cap = 2*sess.cap;
                sess.len = sess.len-sess.start;
                sess.start = 0;
            } else {
                return ERR_HTTP_CONTENT_LIMIT;
            }
        }
    } else {
        return ret;
    }

    return 0;
}

void handleHttp(HttpRequest& req)
{
    if (req.method=="GET")
    {

    }

    sendHttpOK(req.fd);
}

void sendHttpOK(int fd)
{
    std::string ret;
    ret.append("HTTP/1.1 200 OK\r\n",17);
    ret.append("Content-Length: 2\r\n\r\n",21);
    ret.append("OK",2);
    ::send(fd,ret.c_str(),ret.size(),0);
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

void* worker(void* argv)
{

}

int parseHttp(char* buf,int* start_p,int length,HttpRequest& req)
{
    int start = *start_p;
    int headerLen = findHeaderEnd(buf,start,length);
    if (headerLen<0)
        return ERR_HTTP_NOT_COMPLETE;
    int headerEnd = start+headerLen;
    int lineLen = findLine(buf,start,headerEnd+2);
    int lineEnd = start + lineLen;
    int reqMethodLen = findSpace(buf,start,lineEnd);
    if (reqMethodLen<=0)
        return ERR_HTTP_REQ_METHOD;
    req.method.append(buf+start,reqMethodLen);
    start += reqMethodLen+1;
    int reqURILen = findSpace(buf,start,lineEnd);
    if (reqURILen<=0)
        return ERR_HTTP_REQ_URI;
    req.uri.append(buf+start,reqURILen);
    start += reqURILen+1;
    if (start>=lineEnd)
        return ERR_HTTP_REQ_VERSION;
    req.version.append(buf+start,lineEnd-start);

    std::cout << req.method << " " << req.uri << " " << req.version << std::endl;

    if (lineEnd == headerEnd)
    {
        *start_p = lineEnd+4;
        return 0;
    }
    start = lineEnd+2;

    while (start<headerEnd+2)
    {
        int lineLen = findLine(buf,start,headerEnd+2);
        int lineEnd = start + lineLen;
        int headerKeyLen = findSub(buf,start,lineEnd);
        if (headerKeyLen<0)
            return ERR_HTTP_HEADER_KV;
        std::string header_key(buf+start,headerKeyLen);
        std::string header_val(buf+start+headerKeyLen+2,lineLen-headerKeyLen-2);
        req.header.insert(std::pair<std::string,std::string>(header_key,header_val));

        std::cout << "header " << header_key << ":" << header_val << std::endl;
        start = lineEnd+2;
    }

    start += 2;

    // 需要继续解析Body数据
    if (req.method == "POST" && req.header.find("Content-Length")!=req.header.end())
    {
        std::string contentLengthStr = req.header["Content-Length"];
        int contentLength;
        int ret = myAtoi((char*)contentLengthStr.c_str(),contentLengthStr.size(),&contentLength);
        if (ret<0 || contentLength<0)
            return ERR_HTTP_HEADER_CONTENT_LENGTH;

        if (length-start>=contentLength)
        {
            req.body.append(buf+start,contentLength);
            start += contentLength;
            std::cout << "body>" << req.body << std::endl;
        } else {
            return ERR_HTTP_NOT_COMPLETE;
        }
    }

    *start_p = start;
    return 0;
}

int findHeaderEnd(char* buf,int start,int end)
{
    for (int i = start;i<end-3;i++)
    {
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
        {
            return i-start;
        }
    }
    return -1;
}

int findLine(char* buf,int start,int end)
{
    for (int i = start;i<end-1;i++)
    {
        if (buf[i]=='\r' && buf[i+1]=='\n')
        {
            return i-start;
        }
    }
    return -1;
}

int findSpace(char* buf,int start,int end)
{
    for (int i = start;i<end;i++)
    {
        if (buf[i]==' ')
        {
            return i-start;
        }
    }
    return -1;
}

int findSub(char* buf,int start,int end)
{
    for (int i = start;i<end;i++)
    {
        if (buf[i]==':')
        {
            return i-start;
        }
    }
    return -1;
}

int myAtoi(char* p,int end,int* val)
{
    int value= 0;
    int sign = 1;
    if (end<=0)
        return -1;
    for (int i=0;i<end;i++)
    {
        if (p[i]=='-')
            sign = -1;
        else if (p[i]<'0' || p[i]>'9')
            return -2;
        else
            value = value * 10 + p[i]-'0';
    }
    *val = value;
    return 0;
}