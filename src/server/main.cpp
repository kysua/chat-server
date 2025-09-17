#include "chatserver.hpp"
#include "chatservice.hpp"
#include <muduo/base/Logging.h>
#include "ThreadPool.hpp"
#include <iostream>
#include <signal.h>
using namespace std;

// 捕获SIGINT的处理函数
void resetHandler(int)
{
    LOG_INFO << "capture the SIGINT, will reset state\n";
    //ChatService::instance()->reset();
    exit(0);
}

ThreadPool g_threadPool(4); // 比如，创建一个有4个工作线程的线程池
// main.cpp
int main(int argc, char **argv)
{
    if (argc < 3) {
        cerr << "command invalid! example: ./ChatServer <port> <server_name>" << endl;
        exit(-1);
    }

    signal(SIGINT, resetHandler);

    uint16_t port = atoi(argv[1]);
    std::string server_name = argv[2];

    // === 关键修改：在启动前初始化单例 ===
    ChatService::instance()->init(server_name);

    EventLoop loop;
    InetAddress addr(port);
    // ChatServer 构造函数中的名字只是muduo日志用的，与我们的业务逻辑无关
    ChatServer server(&loop, addr, "ChatServer");

    server.start();
    loop.loop();

    return 0;
}