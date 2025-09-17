#pragma once
#ifndef REDISSTATESTORAGE_H
#define REDISSTATESTORAGE_H

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory> // For std::shared_ptr
#include <hiredis/hiredis.h>
#include <unordered_map>

class Connectionguard;
class RedisStateStorage {
public:
    RedisStateStorage(size_t pool_size = 10, const char* ip = "127.0.0.1", int port = 6379);
    ~RedisStateStorage();

    // 禁止拷贝和赋值
    RedisStateStorage(const RedisStateStorage&) = delete;
    RedisStateStorage& operator=(const RedisStateStorage&) = delete;

    bool setUserOnline(const std::string& user_id, const std::string& server_id, int ttl_seconds = 60);
    bool setUserOffline(const std::string& user_id);
    bool getUserStatus(const std::string& user_id, std::string& server_id);
    bool refreshUserTTL(long long user_id, int ttl_seconds = 60);
    std::unordered_map<long long, std::string> getUsersStatus(const std::vector<long long>& user_ids);
    friend class ConnectionGuard;
protected:
    // 获取和归还连接的内部方法
    redisContext* getConnection();
    void releaseConnection(redisContext* conn);
    
private:
    // **设计决策更新**：
    // 使用独立的 KEY (例如 "online_users:1001") 而不是 HASH field,
    // 因为 Redis 的 EXPIRE 命令只能对顶层 KEY 生效, 无法对 HASH 中的字段设置过期时间。
    // 这样才能完美地实现心跳续期功能。
    const std::string _key_prefix = "online_users:";

    size_t _pool_size;
    std::queue<redisContext*> _connections; // 连接队列
    std::mutex _mutex;                      // 互斥锁
    std::condition_variable _cv;            // 条件变量
};

#endif