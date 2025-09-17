#ifndef CHATSERVICE_H
#define CHATSERVICE_H

#include <muduo/net/TcpConnection.h>
#include <unordered_map>
#include <functional>
#include <mutex>
#include "json.hpp"
#include "usermodel.hpp"
#include "offlinemessagemodel.hpp"
#include "friendmodel.hpp"
#include "group_model.hpp"
#include "redisPub.hpp"
#include "RedisStateStorage.hpp"
#include <set>
#include <unordered_set>
#include "ThreadPool.hpp"

using json = nlohmann::json;
using namespace muduo;
using namespace muduo::net;

// 回调函数类型
using MsgHandler = std::function<void(const TcpConnectionPtr&, json&, Timestamp)>;

// 聊天服务器业务类
class ChatService
{
public:
    // ChatService 单例模式
    // thread safe
    static ChatService* instance() {
        static ChatService service;
        return &service;
    }
     void init(const std::string& server_id);

     ThreadPool* getThreadPool();

    // 登录业务
    void loginHandler(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 注册业务
    void registerHandler(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 一对一聊天业务
    void oneChatHandler(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 添加好友业务
    void addFriendHandler(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 获取对应消息的处理器
    MsgHandler getHandler(int msgId);
    // 创建群组业务
    void createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 加入群组业务
    void addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 群组聊天业务
    void groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理客户端异常退出
    void clientCloseExceptionHandler(const TcpConnectionPtr &conn);
    //用户心跳信息
    void heartbeatHandler(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 服务端异常终止之后的操作
    void reset();
    //redis订阅消息触发的回调函数
    void redis_subscribe_message_handler(const string& channel, const string& message);

    void logoutHandler(const TcpConnectionPtr &conn, json &js, Timestamp time);

private:
    ChatService();
    ChatService(const ChatService&) = delete;
    ChatService& operator=(const ChatService&) = delete;

    // 存储消息id和其对应的业务处理方法
    std::unordered_map<int, MsgHandler> _msgHandlerMap;
    
    // 存储在线用户的通信连接
    std::unordered_map<long long, TcpConnectionPtr> _userConnMap;

    //
    std::unordered_map<int, std::unordered_set<long long>> _localGroupCache;
    // 定义互斥锁
    std::mutex _connMutex;

    std::mutex _groupCacheMutex;


    //redis操作对象
    RedisPub _redis;
    std::unique_ptr<RedisStateStorage> _RedisStateStorage;

    std::unique_ptr<ThreadPool> _threadPool;

    // 数据操作类对象
    UserModel _userModel;
    OfflineMsgModel _offlineMsgModel;
    FriendModel _friendModel;
    GroupModel _groupModel;

    std::string my_server_id="localServer";
};

#endif // CHATSERVICE_H