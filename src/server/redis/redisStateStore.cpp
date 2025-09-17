#include "RedisStateStorage.hpp"
#include <iostream>

// RAII 辅助类，用于自动归还连接
// 使得业务逻辑代码更简洁，且异常安全
class ConnectionGuard {
public:
    ConnectionGuard(redisContext** conn_ptr, RedisStateStorage* pool)
        : _conn_ptr(conn_ptr), _pool(pool) {
        *_conn_ptr = _pool->getConnection();
    }
    ~ConnectionGuard() {
        if (*_conn_ptr != nullptr) {
            _pool->releaseConnection(*_conn_ptr);
        }
    }
private:
    redisContext** _conn_ptr;
    RedisStateStorage* _pool;
};


RedisStateStorage::RedisStateStorage(size_t pool_size, const char* ip, int port) : _pool_size(pool_size) {
    for (size_t i = 0; i < _pool_size; ++i) {
        redisContext* conn = redisConnect(ip, port);
        if (conn == nullptr || conn->err) {
            if (conn) {
                std::cerr << "Redis connection error: " << conn->errstr << std::endl;
                redisFree(conn);
            } else {
                std::cerr << "Can't allocate redis context" << std::endl;
            }
            continue; // or throw an exception
        }
        _connections.push(conn);
    }
}

RedisStateStorage::~RedisStateStorage() {
    std::lock_guard<std::mutex> lock(_mutex);
    while (!_connections.empty()) {
        redisContext* conn = _connections.front();
        _connections.pop();
        redisFree(conn);
    }
}

redisContext* RedisStateStorage::getConnection() {
    std::unique_lock<std::mutex> lock(_mutex);
    // 使用 while 循环防止虚假唤醒
    while (_connections.empty()) {
        // 等待，直到 _connections 不为空
        _cv.wait(lock, [this] { return !_connections.empty(); });
    }

    redisContext* conn = _connections.front();
    _connections.pop();
    return conn;
}

void RedisStateStorage::releaseConnection(redisContext* conn) {
    if (conn == nullptr) return;
    std::lock_guard<std::mutex> lock(_mutex);
    _connections.push(conn);
    _cv.notify_one(); // 唤醒一个等待的线程
}

// === 公共接口实现 ===

bool RedisStateStorage::setUserOnline(const std::string& user_id, const std::string& server_id, int ttl_seconds) {
    redisContext* conn = nullptr;
    ConnectionGuard guard(&conn, this); // RAII: 自动获取和释放连接
    if (conn == nullptr) return false;

    // 使用 SET key value EX seconds 命令，原子地设置键、值和过期时间
    redisReply* reply = (redisReply*)redisCommand(conn, "SET %s%s %s EX %d",
                                                   _key_prefix.c_str(), user_id.c_str(),
                                                   server_id.c_str(), ttl_seconds);
    if (reply == nullptr) {
        return false;
    }

    bool success = (reply->type == REDIS_REPLY_STATUS && std::string(reply->str) == "OK");
    freeReplyObject(reply); // 必须释放 reply 对象
    return success;
}

bool RedisStateStorage::setUserOffline(const std::string& user_id) {
    redisContext* conn = nullptr;
    ConnectionGuard guard(&conn, this);
    if (conn == nullptr) return false;

    redisReply* reply = (redisReply*)redisCommand(conn, "DEL %s%s", _key_prefix.c_str(), user_id.c_str());
    if (reply == nullptr) {
        return false;
    }
    
    // DEL 命令返回删除的 key 的数量，成功则为 1
    bool success = (reply->type == REDIS_REPLY_INTEGER && reply->integer >= 0);
    freeReplyObject(reply);
    return success;
}

/**
 * @brief 获取用户的在线状态和所在的服务器ID。
 * 
 * @param user_id 要查询的用户ID。
 * @param server_id [输出参数] 如果用户在线，这里将被填充为服务器的ID。
 * @return true 如果用户在线。
 * @return false 如果用户离线或查询出错。
 */
bool RedisStateStorage::getUserStatus(const std::string& user_id, std::string& server_id) {
    redisContext* conn = nullptr;
    ConnectionGuard guard(&conn, this);
    if (conn == nullptr) {
        return false; // 连接池获取连接失败
    }

    redisReply* reply = (redisReply*)redisCommand(conn, "GET %s%s", _key_prefix.c_str(), user_id.c_str());
    if (reply == nullptr) {
        // 命令执行失败，可能是Redis服务断开
        return false;
    }

    bool is_online = false;
    // 只有当Redis返回字符串类型时，才表示用户在线
    if (reply->type == REDIS_REPLY_STRING) {
        // 将查询到的服务器ID赋值给输出参数
        server_id = std::string(reply->str, reply->len);
        is_online = true;
    }
    // 其他情况 (reply->type 为 REDIS_REPLY_NIL 或 REDIS_REPLY_ERROR) 都视为离线
    
    freeReplyObject(reply); // 释放资源
    return is_online;
}

// RedisStateStorage.cpp
bool RedisStateStorage::refreshUserTTL(long long user_id, int ttl_seconds) {
    redisContext* conn = nullptr;
    ConnectionGuard guard(&conn, this);
    if (conn == nullptr) return false;

    // 将 long long 转换为 string 以便在 redis 命令中使用
    std::string user_id_str = std::to_string(user_id);

    redisReply* reply = (redisReply*)redisCommand(conn, "EXPIRE %s%s %d",
                                                   _key_prefix.c_str(), user_id_str.c_str(), ttl_seconds);
    if (reply == nullptr) {
        return false;
    }
    
    // EXPIRE 命令成功时返回 1 (设置成功)
    bool success = (reply->type == REDIS_REPLY_INTEGER && reply->integer == 1);
    freeReplyObject(reply);
    return success;
}



std::unordered_map<long long, std::string> RedisStateStorage::getUsersStatus(const std::vector<long long>& user_ids) {
    std::unordered_map<long long, std::string> online_users;
    if (user_ids.empty()) {
        return online_users;
    }

    redisContext* conn = nullptr;
    ConnectionGuard guard(&conn, this);
    if (conn == nullptr) return online_users;

    // 1. 构造 MGET 命令的参数列表
    // hiredis 的 redisCommandArgv 需要一个 `const char*` 的数组
    std::vector<const char*> argv;
    std::vector<std::string> keys; // string 用于管理内存，防止 C-string 指针失效

    // 第一个参数是命令本身 "MGET"
    argv.push_back("MGET");
    
    // 准备所有的 key: "online_users:1001", "online_users:1002", ...
    keys.reserve(user_ids.size());
    for (long long user_id : user_ids) {
        keys.push_back(_key_prefix + std::to_string(user_id));
        argv.push_back(keys.back().c_str());
    }

    // 2. 执行 redisCommandArgv 命令
    redisReply* reply = (redisReply*)redisCommandArgv(conn, argv.size(), argv.data(), nullptr);
    if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        return online_users;
    }

    // 3. 解析返回的数组
    // MGET 返回的数组与请求的 key 顺序一一对应
    for (size_t i = 0; i < reply->elements; ++i) {
        // 如果 reply->element[i] 的类型是 REDIS_REPLY_NIL, 说明该 key 不存在 (用户离线)
        if (reply->element[i]->type == REDIS_REPLY_STRING) {
            // 用户在线，将其加入到结果 map 中
            online_users[user_ids[i]] = std::string(reply->element[i]->str, reply->element[i]->len);
        }
    }

    freeReplyObject(reply);
    return online_users;
}