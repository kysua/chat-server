#ifndef USER_H
#define USER_H

#include <string>

class User
{
public:
    User(long long id = -1, std::string name = "", 
        std::string pwd = "")
        : _id(id),
          _name(name)
    {}

    void setId(const long long &id) { _id = id; }
    void setName(const std::string &name) { _name = name; }
    void setPassword(const std::string &paw) { _password = paw; }
    //void setState(const std::string &state) { _state = state; }

    long long getId() const{ return _id; }
    std::string getName()const { return _name; } 
    std::string getPassword()const { return _password; } 
    //std::string getState()const { return _state; } 

protected:
    long long _id;
    std::string _name;
    std::string _password;
    //std::string _state;    
};


#endif // USER_H