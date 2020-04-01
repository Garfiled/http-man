#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <string>
#include <iostream>
#include <time.h>
#include <string.h>

#include "http-parser.h"

#define PORT 8080

void* worker(Session* s);
void sendHttpObj(HttpRequest& req,std::string);
void handleHttp(HttpRequest& req);


/*
bench with ab
ab -n 1 -c 1 -k -v 4 127.0.0.1:8080/
ab -n 200000 -c 10 -k 127.0.0.1:8080/
*/

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
        auto w = new std::thread(worker,sess);
        // 怎么join？

        // std::thread w(worker,sess);
        // 不join会有问题的
    }

    close(socket_fd);

    return 0;
}



void handleHttp(HttpRequest& req)
{

    if (req.method=="GET")
    {

    }

    sendHttpObj(req,"ok");
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

void* worker(Session* input)
{
    Session sess = *input;
    delete input;
    while (true)
    {
        int ret = processQuery(sess);
        if (ret!=0)
        {
            if (ret!=ERR_HTTP_CONNECT_CLOSE && ret!=ERR_HTTP_READ_EOF) {
                std::cout << "processQuery err:" << ret << std::endl;
            }
            break;
        }
    }

    delete sess.buf;

    return nullptr;
}
