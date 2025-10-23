#include "chatserver.hpp"
#include "json.hpp"
#include "chatservice.hpp"
#include <iostream>
#include <functional>
#include <string>
#include <muduo/base/Logging.h>
using namespace std;
using namespace placeholders;
using json = nlohmann::json;


// 初始化聊天服务器对象
ChatServer::ChatServer(EventLoop *loop,
                       const InetAddress &listenAddr,
                       const string &nameArg)
    : _server(loop, listenAddr, nameArg), _loop(loop)
{
    // 注册连接事件的回调函数
    _server.setConnectionCallback(std::bind(&ChatServer::onConnection, this, _1));

    // 注册消息事件的回调函数
    _server.setMessageCallback(std::bind(&ChatServer::onMessage, this, _1, _2, _3));

    // 设置subLoop线程数量
    _server.setThreadNum(4);
}

// 启动服务
void ChatServer::start()
{
    _server.start();
}

// 连接事件相关信息的回调函数
void ChatServer::onConnection(const TcpConnectionPtr &conn)
{
    // 客户端断开连接
    if (!conn->connected())
    {
        // 处理客户端异常退出事件
        ChatService::instance()->clientCloseExceptionHandler(conn);
        // 半关闭
        conn->shutdown();
    }
}

/*
// 上报读写事件相关信息的回调函数
void ChatServer::onMessage(const TcpConnectionPtr &conn,
                           Buffer *buffer,
                           Timestamp time)
{
    // 将json数据转换为string
    string buf = buffer->retrieveAllAsString();
    // 数据的反序列化
    json js = json::parse(buf.c_str());
    
    // 完全解耦网络模块和业务模块，不要在网络模块中调用业务模块的方法
    // 通过 js["msg_id"] 来获取不同的业务处理器（事先绑定的回调方法）
    // js["msgid"].get<int>() 将js["msgid"]对应的值强制转换成int
    auto msgHandler = ChatService::instance()->getHandler(js["msgid"].get<int>());
    // 回调消息绑定好的事件处理器，来执行相应的业务处理
    msgHandler(conn, js, time);
}*/

// 上报读写事件相关信息的回调函数
void ChatServer::onMessage(const TcpConnectionPtr &conn,
                           Buffer *buffer,
                           Timestamp time)
{
    string buf = buffer->retrieveAllAsString();
    json js = json::parse(buf.c_str());
    
    auto msgHandler = ChatService::instance()->getHandler(js["msgid"].get<int>());

    ThreadPool* pool = ChatService::instance()->getThreadPool();
    
    if (pool)
    {
        // ******************* 核心修改 *******************
        // 使用 mutable 关键字，使得按值捕获的变量可以在 lambda 内部被修改
        bool success = pool->enqueue([=]() {
            // 这个lambda捕获了所有需要的上下文，并将在工作线程中执行
            // 注意：这里没有使用 mutable，因为 js 和 conn 都是拷贝/智能指针拷贝，
            // 在lambda内部不需要修改它们自身。
            msgHandler(conn, js, time);
        });

        // 关键：处理任务提交失败的情况
        if (!success)
        {
            // 线程池队列已满，服务器繁忙
            LOG_WARN << "ThreadPool is full, task rejected for user on connection " << conn->name();
            
            // 向客户端发送服务不可用响应
            json response;
            response["msgid"] = -1; // 使用一个特殊的msgid表示错误
            response["errno"] = 503; // 类似HTTP 503 Service Unavailable
            response["errmsg"] = "Server is busy, please try again later.";
            conn->send(response.dump());
            
            // 可以选择不关闭连接，让客户端稍后重试
            // conn->shutdown(); 
        }
        // ***********************************************
    }
    else
    {
        // 线程池未初始化，记录错误
        LOG_ERROR << "ThreadPool is not initialized!";
    }
}
