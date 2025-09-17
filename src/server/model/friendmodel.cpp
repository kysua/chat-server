#include "friendmodel.hpp"
#include "connectPool.hpp"

// 添加好友关系
void FriendModel::insert(long long userId, long long friendId)
{
    // 组织sql语句
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "insert into friend values(%lld, %lld)", userId, friendId);

    //MySQL mysql;
     shared_ptr<MySQL> mysql = ConnectionPool::getInstance()->getConnection();
    if (mysql)
    {
        mysql->update(sql);
    }
}

// 返回用户好友列表
std::vector<User> FriendModel::query(long long userId)
{
    // 1.组装sql语句
    char sql[1024] = {0};

    // 联合查询
    sprintf(sql, "select a.id, a.name from user a inner join friend b on b.friendid = a.id where b.userid=%lld", userId);

    std::vector<User> vec;
    shared_ptr<MySQL> mysql = ConnectionPool::getInstance()->getConnection();
    if (mysql)
    {
        MYSQL_RES *res = mysql->query(sql);
        if (res != nullptr)
        {
            // 把userid用户的所有离线消息放入vec中返回
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr)
            {
                User user;
                user.setId(atoi(row[0]));
                user.setName(row[1]);
                //user.setState(row[2]);
                vec.push_back(user);
            }
            mysql_free_result(res);
            return vec;
        }
    }
    return vec;
}
