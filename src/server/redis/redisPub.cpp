#include "redisPub.hpp"
#include <iostream>
#include <muduo/base/Logging.h>

RedisPub::RedisPub() : publish_context_(nullptr), subcribe_context_(nullptr)
{
}

RedisPub::~RedisPub()
{
    if (publish_context_ != nullptr)
    {
        redisFree(publish_context_);
    }

    if (subcribe_context_ != nullptr)
    {
        redisFree(subcribe_context_);
    }
}

//连接Redis服务器
// redisPub.cpp
bool RedisPub::connect()
{
    publish_context_ = redisConnect("127.0.0.1", 6379);
    if (publish_context_ == nullptr)
    {
        cerr << "connect redis failed!" << endl;
        return false;
    }

    subcribe_context_ = redisConnect("127.0.0.1", 6379);
    if (subcribe_context_ == nullptr)
    {
        cerr << "connect redis failed!" << endl;
        return false;
    }

    // !!! 删除这里的线程创建和启动代码 !!!
    /*
    thread t([&]() {
        observer_channel_message();
    });
    t.detach();
    */

    cout << "connect redis-server success!" << endl;
    return true;
}

//向Redis指定的通道channel发布消息
// redisPub.cpp

bool RedisPub::publish(string channel, string message)
{
    // 使用非阻塞的 redisAppendCommand，它只将命令放入本地缓冲区
    if (REDIS_ERR == redisAppendCommand(publish_context_, "PUBLISH %s %s", channel.c_str(), message.c_str()))
    {
        cerr << "publish command failed: redisAppendCommand" << endl;
        return false;
    }

    // 确保缓冲区中的命令被发送出去
    int done = 0;
    while (!done)
    {
        if (REDIS_ERR == redisBufferWrite(publish_context_, &done))
        {
            cerr << "publish command failed: redisBufferWrite" << endl;
            // (可选) 在这里可以加上连接重试的逻辑
            return false;
        }
    }
    
    // 因为我们不关心 PUBLISH 的返回值，所以我们不需要调用 redisGetReply
    // 这样 I/O 线程就可以立即返回，继续处理其他事件

    return true;
}

// 向Redis指定的通道subscribe订阅消息
// redisPub.cpp
// 完整替换 subscribe 函数
bool RedisPub::subscribe(string channel)
{
    // 1. 将要订阅的频道名保存到成员变量中
    subscribe_channel_ = channel;

    // 2. 启动监听线程。现在线程自己会去执行 SUBSCRIBE 命令
    thread t([this]() { // 注意这里改成了 [this]
        observer_channel_message();
    });
    t.detach();

    return true;
}
//取消订阅
bool RedisPub::unsubscribe(string channel)
{
    //redisCommand 会先把命令缓存到context中，然后调用RedisAppendCommand发送给redis
    //redis执行subscribe是阻塞，不会响应，不会给我们一个reply
    if (REDIS_ERR == redisAppendCommand(subcribe_context_, "UNSUBSCRIBE %s", channel))
    {
        cerr << "subscibe command failed" << endl;
        return false;
    }

    int done = 0;
    while (!done)
    {
        if (REDIS_ERR == redisBufferWrite(subcribe_context_, &done))
        {
            cerr << "subscribe command failed" << endl;
            return false;
        }
    }

    return true;
}

//独立线程中接收订阅通道的消息
// redisPub.cpp

// 完整替换这个函数
// 在 redisPub.cpp 中，完整替换这个函数

void RedisPub::observer_channel_message()
{
    // 这个线程现在完全专用于订阅和接收
    redisReply *reply = nullptr;

    // 3. 在这个线程内部，使用简单的 redisCommand 来执行 SUBSCRIBE
    // 这是阻塞的，但正好是我们想要的，因为它会一直等待消息
    reply = (redisReply*)redisCommand(subcribe_context_, "SUBSCRIBE %s", subscribe_channel_.c_str());

    // 检查订阅命令是否成功。如果成功，hiredis 会自动处理好一切。
    if (reply == nullptr) {
        LOG_ERROR << "SUBSCRIBE command failed.";
        redisFree(subcribe_context_);
        subcribe_context_ = nullptr;
        return;
    }
    // 订阅成功后，hiredis 会返回一个确认信息，我们可以释放它
    freeReplyObject(reply);


    // 4. 进入接收消息的循环
    LOG_INFO << "Observer for channel '" << subscribe_channel_ << "' started. Waiting for messages...";
    
    while (redisGetReply(subcribe_context_, (void **)&reply) == REDIS_OK)
    {
        // 健壮性检查: 确保 reply 是一个包含 3 个元素的数组
        if (reply != nullptr && reply->type == REDIS_REPLY_ARRAY && reply->elements == 3)
        {
            // 检查这是否是一条发布的消息
            if (reply->element[0]->type == REDIS_REPLY_STRING && strcmp(reply->element[0]->str, "message") == 0)
            {
                if (reply->element[1]->str != nullptr && reply->element[2]->str != nullptr)
                {
                    // 调用回调函数，将消息上报给业务层
                    notify_message_handler_(reply->element[1]->str, reply->element[2]->str);
                }
            }
        }

        if (reply != nullptr) {
            freeReplyObject(reply);
        }
    }

    LOG_ERROR << "----------------------- observer_channel_message for channel '" << subscribe_channel_ << "' quit! --------------------------";
    if (subcribe_context_) {
        redisFree(subcribe_context_);
        subcribe_context_ = nullptr;
    }
}
//初始化业务层上报通道消息的回调对象
void RedisPub::init_notify_handler(redis_handler handler)
{
    notify_message_handler_ = handler;
}
