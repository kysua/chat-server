#include "chatservice.hpp"
#include "public.hpp"
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <string>
#include <vector>
#include <any>
#include <thread>
using namespace muduo;
using namespace std;

// int getUserId(json& js) { return js["id"].get<int>(); }
// std::string getUserName(json& js) { return js["name"]; }

ChatService::ChatService()
{
    // 对各类消息处理方法的注册
    _msgHandlerMap.insert({LOGIN_MSG, std::bind(&ChatService::loginHandler, this, _1, _2, _3)});
    _msgHandlerMap.insert({REGISTER_MSG, std::bind(&ChatService::registerHandler, this, _1, _2, _3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG, std::bind(&ChatService::oneChatHandler, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG, std::bind(&ChatService::addFriendHandler, this, _1, _2, _3)});
    // 群组业务管理相关事件处理回调注册
    _msgHandlerMap.insert({CREATE_GROUP_MSG, std::bind(&ChatService::createGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG, std::bind(&ChatService::addGroup, this, _1, _2, _3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG, std::bind(&ChatService::groupChat, this, _1, _2, _3)});
    _msgHandlerMap.insert({HEARTBEAT_MSG, std::bind(&ChatService::heartbeatHandler, this, _1, _2, _3)});
    _msgHandlerMap.insert({LOGINOUT_MSG, std::bind(&ChatService::logoutHandler, this, _1, _2, _3)});

}
ThreadPool* ChatService::getThreadPool()
{
    // unique_ptr 的 get() 方法返回其管理的对象的裸指针
    return _threadPool.get();
}

void ChatService::init(const std::string& server_id) {
    my_server_id = server_id;

    unsigned int thread_num = std::thread::hardware_concurrency();
    _threadPool = std::make_unique<ThreadPool>(thread_num);

     // ================== 新增：初始化 RedisStateStorage ==================
    // 注意：请将这里的IP、端口和连接池大小替换为您的实际配置
    // 您可以将它们放在一个配置文件中读取
    const char* redis_ip = "127.0.0.1";
    int redis_port = 6379;
    size_t pool_size = 4; // 例如，创建一个大小为4的连接池

    // 使用 make_unique 创建 RedisStateStorage 的实例
    _RedisStateStorage = std::make_unique<RedisStateStorage>(pool_size, redis_ip, redis_port);
    LOG_INFO << "RedisStateStorage connection pool initialized.";
    // =================================================================


    // 将 Redis 连接和订阅的逻辑移到这里
    if (_redis.connect()) {
        _redis.init_notify_handler(std::bind(&ChatService::redis_subscribe_message_handler, this, _1, _2));
        // 使用传入的 server_id 进行订阅
        _redis.subscribe(my_server_id); 
        LOG_INFO << "Subscribed to channel: " << my_server_id;
    } else {
        LOG_ERROR << "Failed to connect to Redis.";
    }
}
/**
 * @brief 从Redis消息队列中接收订阅消息的回调函数
 * 
 * @param channel 收到消息的频道（也就是我们自己的 server_id）
 * @param message 完整的消息内容（JSON字符串）
 */
void ChatService::redis_subscribe_message_handler(const string& channel, const string& message)
{
       LOG_INFO << "========== REDIS SUB MSG RECEIVED ==========";
    LOG_INFO << "Channel: " << channel << ", Message: " << message;
    json js;
    
    try {
        js = json::parse(message);
    } catch (const json::parse_error& e) {
        // 记录日志，收到了非法的JSON消息，然后直接返回
        LOG_ERROR << "Failed to parse redis message: " << message;
        return;
    }

    // === 关键修改：使用 if-else if 结构分离不同消息类型的逻辑 ===

    // 检查是否为群聊消息
    if (js.contains("groupid"))
    {
        int groupId = js["groupid"].get<int>();

        lock_guard<mutex> lock(_groupCacheMutex);
        auto it = _localGroupCache.find(groupId);

        if (it != _localGroupCache.end()) {
            // 从缓存中直接获取本服务器上的所有群成员
            const auto& localMemberSet = it->second;

            // 向这些成员转发消息
            lock_guard<mutex> connLock(_connMutex);
            for (long long member_id : localMemberSet) {
                auto conn_it = _userConnMap.find(member_id);
                if (conn_it != _userConnMap.end()) {
                     auto targetConn = conn_it->second;
                    // ================== 核心修改 ==================
                    targetConn->getLoop()->runInLoop([targetConn, message]() {
                        targetConn->send(message);
                    });
                    //conn_it->second->send(message);
                }
                // 注意：这里不再需要处理离线逻辑，发送方已经处理过了
            }
        }
        // 处理完毕，直接返回
        return; 
    }
    // 检查是否为单聊消息
    else if (js.contains("toid"))
    {
        long long toId = js["toid"].get<long long>();
        
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toId);
        if (it != _userConnMap.end())
        {
            // 用户就在本机，发送消息
            //it->second->send(message);
            auto targetConn = it->second;
            // ================== 核心修改 ==================
            // 从 Redis 线程调度回目标连接的 I/O 线程
            targetConn->getLoop()->runInLoop([targetConn, message]() {
                targetConn->send(message);
            });
        }
        else
        {
             LOG_ERROR << "CRITICAL: Received message for user " << toId << " but they are NOT in the local connection map!";
            // 边界情况：消息在路由过程中，用户恰好下线了
            // 此时可以进行一次离线存储作为补偿
            _offlineMsgModel.insert(toId, message);
        }
        // 处理完毕，直接返回
        return;
    }
}
    /*
    json js=
    int toId = js["toid"].get<int>();
    lock_guard<mutex> lock(_connMutex);
    auto it = _userConnMap.find(channel);
    if (it != _userConnMap.end())
    {
        it->second->send(message);
        return;
    }

    //转储离线
    _offlineMsgModel.insert(channel, message);*/


MsgHandler ChatService::getHandler(int msgId)
{
    // 找不到对应处理器的情况
    auto it = _msgHandlerMap.find(msgId);
    if (it == _msgHandlerMap.end())
    {
        // 返回一个默认的处理器(lambda匿名函数，仅仅用作提示)
        return [=](const TcpConnectionPtr &conn, json &js, Timestamp) {
            LOG_ERROR << "msgId: " << msgId << " can not find handler!";
        }; 
    }
  
    return _msgHandlerMap[msgId];
}
/*
// 服务器异常，业务重置方法
void ChatService::reset()
{
    // 将所有online状态的用户，设置成offline
    _userModel.resetState();
    
}*/

/**
 * @brief 处理客户端异常退出的回调函数
 * @param conn 断开的连接
 */
void ChatService::clientCloseExceptionHandler(const TcpConnectionPtr &conn)
{
    // 尝试将 context 转换为 long long* (指向 long long 的指针)
     const long long* user_id_ptr = any_cast<long long>(&conn->getContext());

    // 只有当转换成功 (指针不为 nullptr) 时，才执行后续逻辑
    if (user_id_ptr != nullptr)
    {
        long long user_id = *user_id_ptr; // 从指针解引用得到用户ID

        // 1. 清理本地用户连接表
        {
            lock_guard<mutex> lock(_connMutex);
            _userConnMap.erase(user_id);
        }

        // 2. 清理本地群组缓存
        std::vector<Group> userGroups = _groupModel.queryGroups(user_id);
        {
            lock_guard<mutex> lock(_groupCacheMutex);
            for (const auto& group : userGroups) {
                auto it = _localGroupCache.find(group.getId());
                if (it != _localGroupCache.end()) {
                    it->second.erase(user_id);
                    if (it->second.empty()) {
                        _localGroupCache.erase(it);
                    }
                }
            }
        }
        
        // 3. 更新 Redis 中的全局状态
        _RedisStateStorage->setUserOffline(to_string(user_id));
    }
    // 如果 user_id_ptr 是 nullptr，说明 context 里是别的类型或为空，
    // 意味着这个连接从未成功登录过，我们什么都不用做，直接忽略即可。
}
/*
void ChatService::clientCloseExceptionHandler(const TcpConnectionPtr &conn)
{
    User user;
    // 互斥锁保护
    {
        lock_guard<mutex> lock(_connMutex);
        for (auto it = _userConnMap.begin(); it != _userConnMap.end(); ++it)
        {
            if (it->second == conn)
            {
                // 从map表删除用户的链接信息
                user.setId(it->first);
                _userConnMap.erase(it);
                break;
            }
        }
    }
    
    // 用户注销
    _redis.unsubscribe(user.getId()); 

    // 更新用户的状态信息
    if (user.getId() != -1)
    {
        user.setState("offline");
        _userModel.updateState(user);
    }
}
*/
/**
 * @brief 处理用户注销业务
 */
void ChatService::logoutHandler(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    long long user_id = js["id"].get<long long>();

    // 1. 清理本地用户连接表
    {
        lock_guard<mutex> lock(_connMutex);
        _userConnMap.erase(user_id);
    }

    // 2. 清理本地群组缓存
    std::vector<Group> userGroups = _groupModel.queryGroups(user_id);
    {
        lock_guard<mutex> lock(_groupCacheMutex);
        for (const auto& group : userGroups) {
            auto it = _localGroupCache.find(group.getId());
            if (it != _localGroupCache.end()) {
                it->second.erase(user_id);
                if (it->second.empty()) {
                    _localGroupCache.erase(it);
                }
            }
        }
    }
    
    // 3. 更新 Redis 中的全局状态
    _RedisStateStorage->setUserOffline(to_string(user_id));

    LOG_INFO << "User " << user_id << " logged out.";
}
// 一对一聊天业务
void ChatService::oneChatHandler(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    // 需要接收信息的用户ID
    long long toId = js["toid"].get<long long>();
     string messageToSend = js.dump();
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toId);
        // 确认是在线状态
        if (it != _userConnMap.end())
        {
            // TcpConnection::send() 直接发送消息
            auto targetConn = it->second;

            // ================== 核心修改 ==================
            // 将发送给目标用户的操作，调度到目标用户连接所属的 I/O 线程
            targetConn->getLoop()->runInLoop([targetConn, messageToSend]() {
                targetConn->send(messageToSend);
            });
            return;
        }
    }
    LOG_INFO << "User " << toId << " is not local. Preparing to query Redis state.";
    // 用户在其他主机的情况，publish消息到redis
    //User user = _userModel.query(toId);
    std::string server_id="";
    bool is_online = _RedisStateStorage->getUserStatus(std::to_string(toId), server_id);
    LOG_INFO << "server " << server_id << "  friend  is on?  "<<is_online;
    if(is_online){
        _redis.publish(server_id,js.dump());
        return;
    }
    /*
    if (user.getState() == "online")
    {
        _redis.publish(toId, js.dump());
        return;
    }*/

    // toId 不在线则存储离线消息
    _offlineMsgModel.insert(toId, js.dump());
}

// 添加朋友业务
void ChatService::addFriendHandler(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    long long userId = js["id"].get<long long>();
    long long friendId = js["friendid"].get<long long>();

    // 存储好友信息
    _friendModel.insert(userId, friendId);
    _friendModel.insert(friendId, userId);

    json response;
    response["msgid"] = ADD_FRIEND_MSG_ACK;
    response["errno"] = 0;
    response["friendid"] = friendId;
    //conn->send(response.dump());
     conn->getLoop()->runInLoop([conn, response]() {
        conn->send(response.dump());
    });
}

// 创建群组业务
void ChatService::createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    long long userId = js["id"].get<long long>();
    std::string name = js["groupname"];
    std::string desc = js["groupdesc"];

    // 存储新创建的群组消息
    Group group(-1, name, desc);
    json response; // 准备响应

    if (_groupModel.createGroup(group))
    {
        // 存储群组创建人信息
        _groupModel.addGroup(userId, group.getId(), "creator");
        {
            lock_guard<mutex> lock(_groupCacheMutex);
            _localGroupCache[group.getId()].insert(userId);
        }

        // 新增：向客户端发送创建成功的响应
        response["msgid"] = CREATE_GROUP_MSG_ACK;
        response["errno"] = 0;
        response["groupid"] = group.getId(); // 把新群组的ID发给客户端
        //conn->send(response.dump());
         conn->getLoop()->runInLoop([conn, response]() {
            conn->send(response.dump());
        });
    }
    else
    {
        // 新增：发送失败的响应
        response["msgid"] = CREATE_GROUP_MSG_ACK;
        response["errno"] = 1;
        //conn->send(response.dump());
         conn->getLoop()->runInLoop([conn, response]() {
                conn->send(response.dump());
            });
    }
}

// 加入群组业务
void ChatService::addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    long long userId = js["id"].get<long long>();
    int groupId = js["groupid"].get<int>();
    _groupModel.addGroup(userId, groupId, "normal");
    {
    lock_guard<mutex> lock(_groupCacheMutex);
    _localGroupCache[groupId].insert(userId);
    }

    json response;
    response["msgid"] = ADD_GROUP_MSG_ACK;
    response["errno"] = 0;
    response["groupid"] = groupId;
    //conn->send(response.dump());
    conn->getLoop()->runInLoop([conn, response]() {
        conn->send(response.dump());
    });
}

/**
 * @brief 处理群组聊天业务（重构优化后）
 */
void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    long long userId = js["id"].get<long long>();
    int groupId = js["groupid"].get<int>();
    string messageToSend = js.dump(); // 预先准备好要发送的消息
    // 步骤 1: 从数据库获取所有群组成员的ID。这是唯一的一次DB查询。
    std::vector<long long> userIdVec = _groupModel.queryGroupUsers(userId, groupId);
 
                std::unordered_map<long long, std::string> online_group_users = _RedisStateStorage->getUsersStatus(userIdVec);


    // 步骤 2: 批量处理和分发消息
    // 使用一个map来按服务器ID对远程用户进行分组，key为server_id, value为该服务器上的用户列表
    // 这是为了实现对每个远程服务器只发送一次消息的优化
    std::map<string, std::vector<long long>> remote_users_by_server;

    for (long long id : userIdVec)
    {
        // 不给自己发送消息
        if (id == userId) {
            continue;
        }

        // 优先在本地查找
        {
            lock_guard<mutex> lock(_connMutex);
            auto it = _userConnMap.find(id);
            if (it != _userConnMap.end())
            {
                // 成员就在本机，直接发送
                //it->second->send(js.dump());
                 auto targetConn = it->second;
                // ================== 核心修改 ==================
                targetConn->getLoop()->runInLoop([targetConn, messageToSend]() {
                    targetConn->send(messageToSend);
                });
                continue; // 处理下一个用户
            }
        }

        // 本地未找到，查询Redis获取全局状态
        //string server_id ="";

        if (online_group_users.count(id))
        {
            // 用户在线，但在其他服务器上。将其加入待发送的map中。
            // 这里我们暂时不需要存user_id列表，因为我们决定只发送一次消息
            // 简单地用一个 set 记录需要通知的 server_id 即可
            // (为了演示分组概念，我们仍用map，但实际可以简化)
            remote_users_by_server[online_group_users[id]].push_back(id);
        }
        else
        {
            // 用户不在线，存储离线消息
            _offlineMsgModel.insert(id, js.dump());
        }
    }

    // 步骤 3: 对分组后的远程服务器，每个服务器只发送一次群聊消息
    for (auto const& [server_id, users] : remote_users_by_server)
    {
        // _redisPubSub->publish(server_id, js.dump()); // 假设 publish 接受 int
        _redis.publish(server_id, js.dump()); // 如果 publish 接受 string
    }
}
/*
// 群组聊天业务
void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userId = js["id"].get<int>();
    int groupId = js["groupid"].get<int>();
    std::vector<int> userIdVec = _groupModel.queryGroupUsers(userId, groupId);

    lock_guard<mutex> lock(_connMutex);
    for (int id : userIdVec)
    {
        auto it = _userConnMap.find(id);
        if (it != _userConnMap.end())
        {
            // 转发群消息
            it->second->send(js.dump());
        }
        else
        {
            // 查询toid是否在线
            User user = _userModel.query(id);
            if (user.getState() == "online")
            {
                // 向群组成员publish信息
                _redis.publish(id, js.dump());
            }
            else
            {
                //转储离线消息
                _offlineMsgModel.insert(id, js.dump());
            }
        }
    }
}
*/

/**
 * 登录业务
 * 从json得到用户id
 * 从数据中获取此id的用户，判断此用户的密码是否等于json获取到的密码
 * 判断用户是否重复登录
 * {"msgid":1,"id":13,"password":"123456"}
 * {"errmsg":"this account is using, input another!","errno":2,"msgid":2}
 * @brief 处理登录业务（重构后）
 */
void ChatService::loginHandler(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    long long id = js["id"].get<long long>(); // 使用 long long 保持一致
    std::string password = js["password"];

    // 1. 身份认证：从数据库验证用户名和密码 (此步骤不变)
    User user = _userModel.query(id);
    if (user.getId() != -1 && user.getPassword() == password)
    {
        // 2. 全局在线状态检查：查询 Redis
        string server_id ="";
        if (_RedisStateStorage->getUserStatus(to_string(id), server_id))
        {
            // 该用户已经在线（可能在任何一个服务器节点），拒绝重复登录
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 2;
            response["errmsg"] = "This account is already online, duplicate login is not allowed.";
            //conn->send(response.dump());
             conn->getLoop()->runInLoop([conn, response]() {
                conn->send(response.dump());
            });
        }
        else
        {
            // === 登录成功，开始处理在线状态和业务数据 ===

            // 3a. 绑定连接与用户ID，为了高效处理下线
            conn->setContext(id);

            // 3b. 记录用户在本服务器的连接信息 (线程安全)
            {
                lock_guard<mutex> lock(_connMutex);
                _userConnMap.insert({id, conn});
            }

            // 3c. 宣告全局在线：向Redis写入状态信息，并设置过期时间
            // _my_server_id 是当前服务器实例的ID，应从配置中读取
            _RedisStateStorage->setUserOnline(to_string(id), my_server_id);

            // 3d. 填充本地群组缓存
            std::vector<Group> userGroups = _groupModel.queryGroups(id);
            {
                lock_guard<mutex> lock(_groupCacheMutex);
                for (const auto& group : userGroups) {
                    _localGroupCache[group.getId()].insert(id);
                }
            }

            // 4. 构造成功响应
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 0;
            response["id"] = user.getId();
            response["name"] = user.getName();

            // 4a. 拉取离线消息 (逻辑不变)
            /*std::vector<std::string> offlineMsgs = _offlineMsgModel.query(id);
            if (!offlineMsgs.empty())
            {
                
                response["offlinemsg"] = offlineMsgs;
                _offlineMsgModel.remove(id);
            }*/
           std::vector<std::string> offlineMsgs_str = _offlineMsgModel.query(id);
            if (!offlineMsgs_str.empty())
            {
                // === 修改点 3: 优化JSON结构，避免客户端二次解析 ===
                json offline_msgs_json_array = json::array();
                for(const auto& str : offlineMsgs_str) {
                    // 服务器自己先解析，然后将JSON对象放入数组
                    offline_msgs_json_array.push_back(json::parse(str, nullptr, false));
                }
                response["offlinemsg"] = offline_msgs_json_array;
                _offlineMsgModel.remove(id);
            }

            // 4b. 拉取好友列表并查询其实时状态
            std::vector<User> friends = _friendModel.query(id);
            if (!friends.empty())
            {
                std::vector<long long> friend_ids;
                friend_ids.reserve(friends.size());
                for (const auto& friend_user : friends) {
                    friend_ids.push_back(friend_user.getId());
                }
                std::unordered_map<long long, std::string> online_friends = _RedisStateStorage->getUsersStatus(friend_ids);

                json friends_json_array = json::array();
                for (auto& friend_user : friends)
                {
                    json friend_json;
                    friend_json["id"] = friend_user.getId();
                    friend_json["name"] = friend_user.getName();
                    // 关键: 从Redis查询好友的实时状态
                     if (online_friends.count(friend_user.getId())) {
                        friend_json["state"] = "online";
                    } else {
                        friend_json["state"] = "offline";
                    }
                    friends_json_array.push_back(friend_json);
                }
                response["friends"] = friends_json_array;
            }

            // 4c. 拉取群组信息 (userGroups 已在步骤 3d 获取)
            // 4c. 拉取群组信息 (userGroups 已在步骤 3d 获取)
            if (!userGroups.empty()) {
                // === 优化点：批量获取所有群成员的在线状态 ===

                // 1. 收集所有群组中所有成员的唯一ID
                std::unordered_set<long long> all_member_ids;
                for (auto& group : userGroups) {
                    for (auto& user : group.getUsers()) {
                        all_member_ids.insert(user.getId());
                    }
                }

                // 2. 一次性从 Redis 查询所有这些成员的在线状态
                std::vector<long long> member_id_vec(all_member_ids.begin(), all_member_ids.end());
                std::unordered_map<long long, std::string> online_statuses = _RedisStateStorage->getUsersStatus(member_id_vec);

                // 3. 构建 JSON 响应，从内存map中获取状态
                json groups_json_array = json::array();
                for (auto& group : userGroups) {
                    json group_json;
                    group_json["id"] = group.getId();
                    group_json["name"] = group.getName();
                    group_json["desc"] = group.getDesc();
                    
                    json users_json_array = json::array();
                    for (auto& user : group.getUsers()) {
                        json group_user_json;
                        group_user_json["id"] = user.getId();
                        group_user_json["name"] = user.getName();
                        
                        // 从预先获取的 map 中高效查找状态，而不是再次查询Redis
                        if (online_statuses.count(user.getId())) {
                            group_user_json["state"] = "online";
                        } else {
                            group_user_json["state"] = "offline";
                        }
                        users_json_array.push_back(group_user_json);
                    }
                    group_json["users"] = users_json_array;
                    groups_json_array.push_back(group_json);
                }
                response["groups"] = groups_json_array;
            }
            conn->getLoop()->runInLoop([conn, response]() {
                conn->send(response.dump());
            });
        }
    }
    else
    {
        // 认证失败
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "Invalid username or password!";
        conn->getLoop()->runInLoop([conn, response]() {
                conn->send(response.dump());
            });
    }
}
// 注册业务
void ChatService::registerHandler(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    LOG_DEBUG << "do regidster service!";

    std::string name = js["name"];
    std::string password = js["password"];
    json response;
    User user;
    user.setName(name);
    user.setPassword(password);
    bool state = _userModel.insert(user);
    if (state)
    {
        // 注册成功
        
        response["msgid"] = REGISTER_MSG_ACK;
        response["errno"] = 0;
        response["id"] = user.getId();
        // json::dump() 将序列化信息转换为std::string
        //conn->send(response.dump());
    }
    else
    {
        // 注册失败
        json response;
        response["msgid"] = REGISTER_MSG_ACK;
        response["errno"] = 1;
        // 注册已经失败，不需要在json返回id
        //conn->send(response.dump());
    }
     conn->getLoop()->runInLoop([conn, response]() {
        conn->send(response.dump());
    });
}


/**
 * @brief 处理客户端心跳消息
 */
void ChatService::heartbeatHandler(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    long long userid_from_json = js["id"].get<long long>();
    
    // 同样，使用 muduo 的 any_cast
    const long long* context_userid_ptr = any_cast<long long>(&conn->getContext());

    if (context_userid_ptr != nullptr) 
    {
        long long context_userid = *context_userid_ptr;
        if (context_userid == userid_from_json) {
             bool success = _RedisStateStorage->refreshUserTTL(context_userid, 60);
             if (!success) {
                LOG_INFO << "TTL refresh failed for user " << context_userid << ", maybe already offline.";
             }
        }
    }
}