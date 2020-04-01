#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>

#include <string>
#include <iostream>
#include <vector>
#include <map>

#include <time.h>

#define PORT 8080

#define CONTENT_LEN_LIMIT 500*1024*1024

#define ERR_HTTP_NOT_COMPLETE 100001
#define ERR_HTTP_REQ_METHOD   100002
#define ERR_HTTP_REQ_URI      100003
#define ERR_HTTP_REQ_VERSION  100004
#define ERR_HTTP_HEADER_KV    100005
#define ERR_HTTP_HEADER_CONTENT_LENGTH    100006
#define ERR_HTTP_CONTENT_LIMIT            100007
#define ERR_HTTP_CONNECT_CLOSE  100008

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
int processQuery(Session&);

// 解析http协议
int parseHttp(char* buf,int* start_p,int length,HttpRequest& req);
int findHeaderEnd(char* buf,int start,int end);
int findLine(char* buf,int start,int end);
int findSpace(char* buf,int start,int end);
int findSub(char* buf,int start,int end);

int myAtoi(char* p,int end,int* val);

void sendHttpOK(HttpRequest& req);
void handleHttp(HttpRequest& req);

int main(int argc, char const *argv[])
{
    int socket_fd;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(struct sockaddr_in);

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
        perror("listen failed");
        exit(EXIT_FAILURE);
    }

    bzero(&address, addrlen);

    std::cout << "server start " << PORT << std::endl;

    while (true)
    {
        int new_fd = accept(socket_fd, (struct sockaddr *) &address, &addrlen);

        if (new_fd < 0)
        {
            std::cout << "accept error " << new_fd << std::endl;
            continue;
        }
        Session* sess = new Session(new_fd,4096);
        pthread_t* pid = new pthread_t();
        int ret = pthread_create(pid,NULL,worker,(void*)sess);
        if (ret!=0)
        {
            perror("create thread failed");
            break;
        }
    }

    close(socket_fd);

    return 0;
}

int processQuery(Session& sess)
{
    int n = read(sess.fd,sess.buf+sess.len,sess.cap - sess.len);
    std::cout << "processQuery:" << n << " " << sess.cap << std::endl;
    if (n<=0)
    {
        return -1;
    }
    sess.len += n;

    int start=0;
    HttpRequest req;
    req.fd = sess.fd;
    int ret = parseHttp(sess.buf,&start,sess.len,req);

    if (ret == 0)
    {
        sess.start = start;
        if (start >= sess.len)
        {
            sess.len = 0;
            sess.start = 0;
        }

        // 命令解析完成，可以交给worker线程处理，这里暂时本地处理
        handleHttp(req);

        if (req.version == "HTTP/1.0" && req.header["Connection"] != "Keep-Alive") {
            std::cout << "close fd" << std::endl;
            close(req.fd);
        }
        return ERR_HTTP_CONNECT_CLOSE;

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

    sendHttpOK(req);
}

void sendHttpOK(HttpRequest& req)
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

    ret.append("Content-Length: 0\r\n",19);
    ret.append("Content-Type: text/plain; charset=utf-8\r\n",41);
    std::cout << (req.version=="HTTP/1.0") << " " << (req.header["Connection"] == "Keep-Alive") << std::endl;
    if (req.version=="HTTP/1.0" && req.header["Connection"] == "Keep-Alive") {
        ret.append("Connection: keep-alive\r\n\r\n",26);
    } else {
        ret.append("\r\n",2);
    }
    // ret.append("ok",2);

    std::cout << ret << std::endl;
    ::send(req.fd,ret.c_str(),ret.size(),0);
    // std::cout << "sendHttpOK:" << fd << std::endl;
}

void* worker(void* argv)
{
    Session *sess_ptr = (Session*)argv;
    Session sess = *sess_ptr;
    delete sess_ptr;
    while (true)
    {
        int ret = processQuery(sess);
        if (ret!=0)
        {
            if (ret!=ERR_HTTP_CONNECT_CLOSE) {
                std::cout << "processQuery err:" << ret << std::endl;
            }
            break;
        }
    }

    delete sess.buf;

    return nullptr;
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

   // std::cout << req.method << " " << req.uri << " " << req.version << std::endl;

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
        // std::cout << "header: " << header_key << " " << header_val << std::endl; 
        req.header.insert(std::pair<std::string,std::string>(header_key,header_val));

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