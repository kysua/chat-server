#ifndef OFFLINE_MESSAGE_MODEL_H
#define OFFLINE_MESSAGE_MODEL_H

#include <string>
#include <vector>
using namespace std;

// 提供离线消息表的操作接口方法
class OfflineMsgModel
{
public:
    // 存储用户的离线消息
    void insert(long long userId, std::string msg);

    // 删除用户的离线消息
    void remove(long long userId);

    // 查询用户的离线消息
    std::vector<std::string> query(long long userId);
};

#endif // OFFLINE_MESSAGE_MODEL_H