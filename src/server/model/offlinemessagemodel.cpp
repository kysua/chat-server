#include "offlinemessagemodel.hpp"
#include "connectPool.hpp"
// 存储用户的离线消息
void OfflineMsgModel::insert(long long userId, std::string msg)
{
    // 组织sql语句
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "insert into offlinemessage values(%lld, '%s')", userId, msg.c_str());

    shared_ptr<MySQL> mysql = ConnectionPool::getInstance()->getConnection();
    if (mysql)
    {
        mysql->update(sql);
    }
}

// 删除用户的离线消息
void OfflineMsgModel::remove(long long userId)
{
    // 组织sql语句
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "delete from offlinemessage where userid=%lld", userId);

    shared_ptr<MySQL> mysql = ConnectionPool::getInstance()->getConnection();
    if (mysql)
    {
        mysql->update(sql);
    }
}

// 查询用户的离线消息
std::vector<std::string> OfflineMsgModel::query(long long userId)
{
    // 组织sql语句
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "select message from offlinemessage where userid = %lld", userId);

    std::vector<std::string> vec;
    shared_ptr<MySQL> mysql = ConnectionPool::getInstance()->getConnection();
    if (mysql)
    {
        MYSQL_RES *res = mysql->query(sql);
        if (res != nullptr)
        {
            // 把userid用户的所有消息放入vec中返回
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                vec.push_back(row[0]);
            }
            mysql_free_result(res);
            return vec;
        }
    }
    return vec;
}
