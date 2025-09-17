#include "json.hpp"
#include <iostream>
#include <thread>
#include <string>
#include <vector>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <limits> // for numeric_limits

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "group.hpp"
#include "user.hpp"
#include "public.hpp"

using json = nlohmann::json;

// ===================================================================================
//  服务器JSON结构优化建议 (需要你在服务器 loginHandler 中同步修改)
// 
//  你的旧结构: response["friends"] = vector<string>  (每个string是序列化的json)
//  推荐的新结构: response["friends"] = json::array() (一个包含多个json对象的数组)
//
//  服务器端修改示例 (loginHandler):
//  json friends_json_array = json::array();
//  for (auto& friend_user : friends) {
//      json friend_json;
//      friend_json["id"] = friend_user.getId();
//      friend_json["name"] = friend_user.getName();
//      friend_json["state"] = ...
//      friends_json_array.push_back(friend_json);
//  }
//  response["friends"] = friends_json_array;
//  这样做客户端解析会极其简单和高效。下面的代码将按照这个优化后的结构来编写。
// ===================================================================================


// ChatClient 客户端类
class ChatClient {
public:
    // 连接服务器
    bool connect(const char* ip, uint16_t port) {
        _clientfd = socket(AF_INET, SOCK_STREAM, 0);
        if (-1 == _clientfd) return false;

        sockaddr_in server;
        memset(&server, 0, sizeof(sockaddr_in));
        server.sin_family = AF_INET;
        server.sin_port = htons(port);
        server.sin_addr.s_addr = inet_addr(ip);

        if (-1 == ::connect(_clientfd, (sockaddr *)&server, sizeof(sockaddr_in))) {
            close(_clientfd);
            _clientfd = -1;
            return false;
        }
        return true;
    }

    // 运行客户端主业务
    void run() {
        if (_clientfd == -1) {
            std::cerr << "Connection failed." << std::endl;
            return;
        }

        // 启动接收线程
        std::thread readTask(&ChatClient::readTaskHandler, this);
        readTask.detach();

        // 启动心跳线程
        std::thread heartbeatTask(&ChatClient::heartbeatTask, this);
        heartbeatTask.detach();

        // main线程用于根据登录状态显示不同菜单
        while (true) {
            if (_isLoggedIn) {
                mainMenu();
            } else {
                loginMenu();
            }
        }
    }

private:
    // 登录菜单
    void loginMenu() {
        std::cout << "========================" << std::endl;
        std::cout << "1. login" << std::endl;
        std::cout << "2. register" << std::endl;
        std::cout << "3. quit" << std::endl;
        std::cout << "========================" << std::endl;
        std::cout << "choice:";
        
        int choice = 0;
        std::cin >> choice;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); // 安全地清空输入缓冲区

        switch (choice) {
        case 1: { // login
            long long id = 0;
            std::string pwd;
            std::cout << "userid:";
            std::cin >> id;
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "userpassword:";
            std::getline(std::cin, pwd);

            json js;
            js["msgid"] = LOGIN_MSG;
            js["id"] = id;
            js["password"] = pwd;
            
            // 等待网络响应
            {
                std::unique_lock<std::mutex> lock(_responseMutex);
                sendJson(js);
                _responseCv.wait(lock);
            }
            break;
        }
        case 2: { // register
            std::string name, pwd;
            std::cout << "username:";
            std::getline(std::cin, name);
            std::cout << "userpassword:";
            std::getline(std::cin, pwd);

            json js;
            js["msgid"] = REGISTER_MSG;
            js["name"] = name;
            js["password"] = pwd;
            
            // 等待网络响应
            {
                std::unique_lock<std::mutex> lock(_responseMutex);
                sendJson(js);
                _responseCv.wait(lock);
            }
            break;
        }
        case 3: // quit
            close(_clientfd);
            exit(0);
        default:
            std::cerr << "invalid input!" << std::endl;
            break;
        }
    }

    // 主菜单
    void mainMenu() {
        help();
        std::string commandbuf;
        while (_isLoggedIn) {
            std::getline(std::cin, commandbuf);
            if (std::cin.eof() || ! _isLoggedIn) { // 处理登出或Ctrl+D的情况
                 break;
            }

            std::string command;
            size_t idx = commandbuf.find(":");
            std::string command_args;

            if (idx == std::string::npos) {
                command = commandbuf;
                command_args = "";
            } else {
                command = commandbuf.substr(0, idx);
                command_args = commandbuf.substr(idx + 1);
            }

            auto it = _commandHandlerMap.find(command);
            if (it == _commandHandlerMap.end()) {
                std::cerr << "invalid input command!" << std::endl;
            } else {
                it->second(command_args);
            }
        }
    }

    // === 业务处理回调 ===
    void doRegResponse(json &responsejs) {
        if (0 != responsejs["errno"].get<int>()) {
            std::cerr << "name is already exist, register error!" << std::endl;
        } else {
            std::cout << "name register success, userid is " << responsejs["id"].get<long long>()
                 << ", do not forget it!" << std::endl;
        }
    }

    void doLoginResponse(json &responsejs) {
        if (0 != responsejs["errno"].get<int>()) {
            std::cerr << responsejs["errmsg"] << std::endl;
            _isLoggedIn = false;
        } else {
            _currentUser.setId(responsejs["id"].get<long long>());
            _currentUser.setName(responsejs["name"]);

            if (responsejs.contains("friends")) {
                _currentUserFriendList.clear();
                for (auto &friend_js : responsejs["friends"]) {
                    User user;
                    user.setId(friend_js["id"].get<long long>());
                    user.setName(friend_js["name"]);
                    //user.setState(friend_js["state"]);
                    _currentUserFriendList.push_back(user);
                }
            }

            if (responsejs.contains("groups")) {
                _currentUserGroupList.clear();
                for (auto &group_js : responsejs["groups"]) {
                    Group group;
                    group.setId(group_js["id"].get<int>());
                    group.setName(group_js["name"]);
                    group.setDesc(group_js["desc"]);

                    if(group_js.contains("users")) {
                        for (auto &user_js : group_js["users"]) {
                            GroupUser user;
                            user.setId(user_js["id"].get<long long>());
                            user.setName(user_js["name"]);
                            //user.setState(user_js["state"]);
                            // user.setRole(user_js["role"]);
                            group.getUsers().push_back(user);
                        }
                    }
                    _currentUserGroupList.push_back(group);
                }
            }

            showCurrentUserData();

            if (responsejs.contains("offlinemsg")) {
                // 现在 offlinemsg 直接就是一个JSON对象数组，可以直接遍历和使用
                for (auto &js : responsejs["offlinemsg"]) {
                    printMessage(js); // 无需二次解析
                }
            }
            _isLoggedIn = true; // 标记为登录成功
        }
    }

    // === 接收和心跳线程 ===
    void readTaskHandler() {
        while (true) {
            char buffer[4096] = {0};
            int len = recv(_clientfd, buffer, sizeof(buffer), 0);
            if (len <= 0) {
                std::cerr << "Server disconnected." << std::endl;
                close(_clientfd);
                exit(0);
            }

            try {
                json js = json::parse(buffer);
                int msgtype = js["msgid"].get<int>();

                if (ONE_CHAT_MSG == msgtype || GROUP_CHAT_MSG == msgtype) {
                    printMessage(js);
                } else if (LOGIN_MSG_ACK == msgtype) {
                    doLoginResponse(js);
                    _responseCv.notify_one(); // 通知主线程
                } else if (REGISTER_MSG_ACK == msgtype) {
                    doRegResponse(js);
                    _responseCv.notify_one(); // 通知主线程
                } else if (CREATE_GROUP_MSG_ACK == msgtype) {
                    if (js["errno"].get<int>() == 0) {
                        std::cout << "\nGroup created successfully! Group ID: " << js["groupid"].get<int>() << std::endl;
                    } else {
                        std::cout << "\nFailed to create group." << std::endl;
                    }
                }else if (ADD_GROUP_MSG_ACK == msgtype) {
                    if (js["errno"].get<int>() == 0) {
                        std::cout << "\nSuccessfully joined group: " << js["groupid"].get<int>() << std::endl;
                    } else {
                        std::cout << "\nFailed to join group." << std::endl;
                    }
                }else if (ADD_FRIEND_MSG_ACK == msgtype) {
                    if (js["errno"].get<int>() == 0) {
                        std::cout << "\nFriend request sent/accepted for user: " << js["friendid"].get<long long>() << std::endl;
                    } else {
                        std::cout << "\nFailed to add friend." << std::endl;
                    }
                }
        
            } catch (const json::parse_error& e) {
                 // 不打印错误，因为TCP粘包可能导致解析失败，这是预期的
                 // 更好的解决方案是实现一个完整的应用层协议（如长度+内容）
            }
        }
    }

    void heartbeatTask() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(25));
            if (_isLoggedIn) {
                json js;
                js["msgid"] = HEARTBEAT_MSG;
                js["id"] = _currentUser.getId();
                sendJson(js);
            }
        }
    }

    // === 辅助函数 ===
    void sendJson(const json& js) {
        std::string request = js.dump();
        if (send(_clientfd, request.c_str(), request.length(), 0) == -1) {
            std::cerr << "send msg error:" << request << std::endl;
        }
    }

    void printMessage(const json& js) {
        if (js["msgid"].get<int>() == ONE_CHAT_MSG) {
            std::cout << "\n" << js["time"].get<std::string>() << " [" << js["id"].get<long long>() << "]" << js["name"].get<std::string>()
                 << " said: " << js["msg"].get<std::string>() << std::endl;
        } else {
            std::cout << "\nGroup[" << js["groupid"].get<int>() << "]:" << js["time"].get<std::string>() << " [" << js["id"].get<long long>() << "]" << js["name"].get<std::string>()
                 << " said: " << js["msg"].get<std::string>() << std::endl;
        }
    }
    
    std::string getCurrentTime() {
        auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ptm;
        localtime_r(&tt, &ptm); // 使用线程安全的 localtime_r
        char date[60] = {0};
        sprintf(date, "%d-%02d-%02d %02d:%02d:%02d",
                (int)ptm.tm_year + 1900, (int)ptm.tm_mon + 1, (int)ptm.tm_mday,
                (int)ptm.tm_hour, (int)ptm.tm_min, (int)ptm.tm_sec);
        return std::string(date);
    }
    
    void showCurrentUserData() {
        std::cout << "======================login user======================" << std::endl;
        std::cout << "current login user => id:" << _currentUser.getId() << " name:" << _currentUser.getName() << std::endl;
        std::cout << "----------------------friend list---------------------" << std::endl;
        if (!_currentUserFriendList.empty()) {
            for (User &user : _currentUserFriendList) {
                std::cout << user.getId() << " " << user.getName() << " " << std::endl;
            }
        }
        std::cout << "----------------------group list----------------------" << std::endl;
        if (!_currentUserGroupList.empty()) {
            for (Group &group : _currentUserGroupList) {
                std::cout << group.getId() << " " << group.getName() << " " << group.getDesc() << std::endl;
                for (GroupUser &user : group.getUsers()) {
                    std::cout << "  - " << user.getId() << " " << user.getName()<< std::endl;
                         // << " " << user.getRole() << std::endl;
                }
            }
        }
        std::cout << "======================================================" << std::endl;
    }

    // === 命令处理 ===
    void help(const std::string& args = "") {
        std::cout << "show command list >>> " << std::endl;
        for (auto &p : _commandMap) {
            std::cout << p.first << " : " << p.second << std::endl;
        }
        std::cout << std::endl;
    }

    void chat(const std::string& str) {
        size_t idx = str.find(":");
        if (idx == std::string::npos || idx == 0 || idx == str.length() - 1) {
            std::cerr << "Invalid chat format! Use: chat:friendid:message" << std::endl;
            return;
        }
        long long friendid = std::stoll(str.substr(0, idx));
        std::string message = str.substr(idx + 1);
        json js;
        js["msgid"] = ONE_CHAT_MSG;
        js["id"] = _currentUser.getId();
        js["name"] = _currentUser.getName();
        js["toid"] = friendid;
        js["msg"] = message;
        js["time"] = getCurrentTime();
        sendJson(js);
    }

    void addfriend(const std::string& str) {
        try {
            long long friendid = std::stoll(str);
            json js;
            js["msgid"] = ADD_FRIEND_MSG;
            js["id"] = _currentUser.getId();
            js["friendid"] = friendid;
            sendJson(js);
        } catch(const std::invalid_argument& e) {
            std::cerr << "Invalid friend id!" << std::endl;
        }
    }

    void creategroup(const std::string& str) {
        size_t idx = str.find(":");
        if (idx == std::string::npos || idx == 0 || idx == str.length() - 1) {
            std::cerr << "Invalid creategroup format! Use: creategroup:groupname:groupdesc" << std::endl;
            return;
        }
        std::string groupname = str.substr(0, idx);
        std::string groupdesc = str.substr(idx + 1);
        json js;
        js["msgid"] = CREATE_GROUP_MSG;
        js["id"] = _currentUser.getId();
        js["groupname"] = groupname;
        js["groupdesc"] = groupdesc;
        sendJson(js);
    }

    void addgroup(const std::string& str) {
        try {
            int groupid = std::stoi(str);
            json js;
            js["msgid"] = ADD_GROUP_MSG;
            js["id"] = _currentUser.getId();
            js["groupid"] = groupid;
            sendJson(js);
        } catch(const std::invalid_argument& e) {
            std::cerr << "Invalid group id!" << std::endl;
        }
    }

    void groupchat(const std::string& str) {
        size_t idx = str.find(":");
        if (idx == std::string::npos || idx == 0 || idx == str.length() - 1) {
            std::cerr << "Invalid groupchat format! Use: groupchat:groupid:message" << std::endl;
            return;
        }
        int groupid = std::stoi(str.substr(0, idx));
        std::string message = str.substr(idx + 1);
        json js;
        js["msgid"] = GROUP_CHAT_MSG;
        js["id"] = _currentUser.getId();
        js["name"] = _currentUser.getName();
        js["groupid"] = groupid;
        js["msg"] = message;
        js["time"] = getCurrentTime();
        sendJson(js);
    }

    void loginout(const std::string& args = "") {
        json js;
        js["msgid"] = LOGINOUT_MSG;
        js["id"] = _currentUser.getId();
        sendJson(js);
        _isLoggedIn = false; // 标记为登出
        _currentUserFriendList.clear();
        _currentUserGroupList.clear();
    }
    
private:
    // 数据成员
    int _clientfd = -1;
    std::atomic_bool _isLoggedIn{false};
    User _currentUser;
    std::vector<User> _currentUserFriendList;
    std::vector<Group> _currentUserGroupList;

    // 线程同步
    std::mutex _responseMutex;
    std::condition_variable _responseCv;

    // 命令映射
    std::unordered_map<std::string, std::string> _commandMap = {
        {"help", "显示所有支持的命令，格式help"},
        {"chat", "一对一聊天，格式chat:friendid:message"},
        {"addfriend", "添加好友，格式addfriend:friendid"},
        {"creategroup", "创建群组，格式creategroup:groupname:groupdesc"},
        {"addgroup", "加入群组，格式addgroup:groupid"},
        {"groupchat", "群聊，格式groupchat:groupid:message"},
        {"loginout", "注销，格式loginout"}};

    std::unordered_map<std::string, std::function<void(std::string)>> _commandHandlerMap = {
        {"help", std::bind(&ChatClient::help, this, std::placeholders::_1)},
        {"chat", std::bind(&ChatClient::chat, this, std::placeholders::_1)},
        {"addfriend", std::bind(&ChatClient::addfriend, this, std::placeholders::_1)},
        {"creategroup", std::bind(&ChatClient::creategroup, this, std::placeholders::_1)},
        {"addgroup", std::bind(&ChatClient::addgroup, this, std::placeholders::_1)},
        {"groupchat", std::bind(&ChatClient::groupchat, this, std::placeholders::_1)},
        {"loginout", std::bind(&ChatClient::loginout, this, std::placeholders::_1)}};
};


int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "command invalid! example: ./ChatClient 127.0.0.1 6000" << std::endl;
        exit(-1);
    }

    ChatClient client;
    if (client.connect(argv[1], atoi(argv[2]))) {
        client.run();
    } else {
        std::cerr << "connect server error" << std::endl;
    }
    
    return 0;
}