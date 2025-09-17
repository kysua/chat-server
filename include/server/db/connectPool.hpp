// connectPool.hpp

#ifndef CONNECT_POOL_HPP
#define CONNECT_POOL_HPP

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <atomic>   // for std::atomic
#include <thread>   // for std::thread
#include <chrono>   // for time points

using namespace std;

// MySQL 包装类 (不变)
class MySQL
{
public:
    MySQL(MYSQL* conn);
    ~MySQL();
    bool update(string sql);
    MYSQL_RES *query(string sql);
    MYSQL* getConnection() const;

private:
    MYSQL *_conn;
};

// 数据库连接池类 (单例)
class ConnectionPool
{
public:
    static ConnectionPool* getInstance();
    shared_ptr<MySQL> getConnection();

private:
    ConnectionPool();
    ~ConnectionPool();
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    bool loadConfigFile();
    
    // 新增的后台线程任务函数
    void produceConnectionTask();
    void scannerConnectionTask();

    string _ip;
    unsigned short _port;
    string _user;
    string _password;
    string _dbname;

    int _initSize;
    int _maxSize;
    int _maxIdleTime;
    int _connectionTimeout;

    // 队列中存放 MYSQL* 和其进入空闲状态的时间点
    queue<pair<MYSQL*, chrono::steady_clock::time_point>> _connectionQueue; 
    mutex _queueMutex;
    condition_variable _cv;
    condition_variable _producer;

    // 新增成员
    atomic<int> _connectionCount; // 记录已创建的连接总数
    bool _stop; // 停止后台线程的标志
};

#endif