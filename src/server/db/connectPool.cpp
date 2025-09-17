#include "connectPool.hpp"
#include <muduo/base/Logging.h>
#include <thread>
#include <functional>

/////////////////////////////////////////////////////////////////////
//          MySQL 类的实现
/////////////////////////////////////////////////////////////////////

MySQL::MySQL(MYSQL* conn) : _conn(conn) {}

MySQL::~MySQL()
{
    // 注意：这里的析构函数不再需要 mysql_close(_conn)
    // 连接的关闭由 ConnectionPool 的析构函数统一管理
}

bool MySQL::update(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_INFO << __FILE__ << ":" << __LINE__ << ":"
                 << sql << " 更新失败! error: " << mysql_error(_conn);
        return false;
    }
    return true;
}

MYSQL_RES* MySQL::query(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_INFO << __FILE__ << ":" << __LINE__ << ":"
                 << sql << " 查询失败! error: " << mysql_error(_conn);
        return nullptr;
    }
    return mysql_use_result(_conn);
}

MYSQL* MySQL::getConnection() const
{
    return _conn;
}


/////////////////////////////////////////////////////////////////////
//          ConnectionPool 类的实现
/////////////////////////////////////////////////////////////////////



ConnectionPool* ConnectionPool::getInstance()
{
    static ConnectionPool pool;
    return &pool;
}

bool ConnectionPool::loadConfigFile()
{
    // ... 保持不变 ...
    _ip = "127.0.0.1";
    _port = 3306;
    _user = "root";
    _password = "123456";
    _dbname = "chat";
    _initSize = 4;
    _maxSize = 16;
    _maxIdleTime = 60; // 单位：秒
    _connectionTimeout = 1000; // 单位：毫秒
    return true;
}

ConnectionPool::ConnectionPool() : _connectionCount(0), _stop(false)
{
    if (!loadConfigFile()) {
        LOG_ERROR << "load db config file failed!";
        return;
    }

    // 创建初始数量的连接
    for (int i = 0; i < _initSize; ++i) {
        MYSQL *p = mysql_init(nullptr);
        if (p && mysql_real_connect(p, _ip.c_str(), _user.c_str(), _password.c_str(), 
                                   _dbname.c_str(), _port, nullptr, 0)) {
            mysql_set_character_set(p, "utf8");
            _connectionQueue.push({p, chrono::steady_clock::now()});
            _connectionCount++;
        } else {
            if (p) mysql_close(p);
            LOG_ERROR << "create initial mysql connection failed!";
        }
    }

    // 启动一个新的线程，作为连接的生产者
    thread produce(std::bind(&ConnectionPool::produceConnectionTask, this));
    produce.detach();

    // 启动一个新的定时线程，扫描多余的空闲连接，进行回收
    thread scanner(std::bind(&ConnectionPool::scannerConnectionTask, this));
    scanner.detach();
}

ConnectionPool::~ConnectionPool()
{
    _stop = true;
    // 唤醒所有等待的线程，以便它们可以检查 _stop 标志并退出
    _cv.notify_all(); 

    lock_guard<mutex> lock(_queueMutex);
    while(!_connectionQueue.empty())
    {
        MYSQL* p = _connectionQueue.front().first;
        _connectionQueue.pop();
        mysql_close(p);
    }
}

// 生产者线程
void ConnectionPool::produceConnectionTask()
{
    while (!_stop)
    {
        unique_lock<mutex> lock(_queueMutex);
        // 只有当队列为空，且总连接数小于最大值时，才生产
        // 如果队列不为空，则生产者应该睡眠，等待被消费者唤醒
        _producer.wait(lock, [&]() {
            return _connectionQueue.size() < _initSize || _connectionCount >= _maxSize || _stop;
        });

        if (_stop) {
            break;
        }

        // 队列为空，可以生产新连接
        if (_connectionCount < _maxSize) {
            MYSQL *p = mysql_init(nullptr);
            if (p && mysql_real_connect(p, _ip.c_str(), _user.c_str(), _password.c_str(), 
                                       _dbname.c_str(), _port, nullptr, 0)) {
                mysql_set_character_set(p, "utf8");
                _connectionQueue.push({p, chrono::steady_clock::now()});
                _connectionCount++;
                // 生产出一个，通知消费者可以来取了
                _cv.notify_all(); 
            } else {
                 if (p) mysql_close(p);
                 // 稍等片刻再尝试，避免因数据库瞬间无法连接而导致CPU空转
                 this_thread::sleep_for(chrono::milliseconds(500));
            }
        } else {
            // 已达到最大连接数，无需生产，继续等待
        }
    }
}

// 扫描和回收线程
void ConnectionPool::scannerConnectionTask()
{
    while (!_stop)
    {
        // 定时醒来，比如每 _maxIdleTime 秒
        this_thread::sleep_for(chrono::seconds(_maxIdleTime));

        if (_stop) {
            break;
        }

        lock_guard<mutex> lock(_queueMutex);
        // 扫描整个队列，释放多余的连接
        while (_connectionCount > _initSize && !_connectionQueue.empty())
        {
            auto& conn_pair = _connectionQueue.front();
            auto idle_duration = chrono::duration_cast<chrono::seconds>(chrono::steady_clock::now() - conn_pair.second);
            
            // 如果连接的空闲时间超过了阈值
            if (idle_duration.count() >= _maxIdleTime)
            {
                MYSQL* p = conn_pair.first;
                _connectionQueue.pop();
                mysql_close(p);
                _connectionCount--;
            }
            else
            {
                // 队头的连接都还没超时，后面的更不会，直接退出本次扫描
                break;
            }
        }
    }
}


shared_ptr<MySQL> ConnectionPool::getConnection()
{
    unique_lock<mutex> lock(_queueMutex);
    
    // 如果队列为空，等待
    while (_connectionQueue.empty()) {
        // 唤醒生产者
        if (_connectionCount < _maxSize) {
             _producer.notify_one();
        }
        if (_cv.wait_for(lock, chrono::milliseconds(_connectionTimeout)) == cv_status::timeout) {
            LOG_INFO << "get mysql connection timeout!";
            return nullptr;
        }
    }

    // 从队列中取出一个连接
    MYSQL* conn = _connectionQueue.front().first;
    _connectionQueue.pop();

    // 使用自定义删除器，当 shared_ptr 析构时，将连接归还给连接池
    shared_ptr<MySQL> sp(new MySQL(conn), [this](MySQL* pconn){
        if (pconn == nullptr) return;
        
        lock_guard<mutex> lock(_queueMutex);
        // 归还时，记录当前时间点
        _connectionQueue.push({pconn->getConnection(), chrono::steady_clock::now()});
        // 通知其他可能在等待连接的线程
        _cv.notify_one(); 
    });
    
    return sp;
}