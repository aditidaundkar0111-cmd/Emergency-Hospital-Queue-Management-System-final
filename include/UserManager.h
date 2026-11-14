#pragma once

#include <map>
#include <string>

class DatabaseManager;

struct AuthUser {
    std::string fullName;
    std::string username;
};

struct AuthSession {
    AuthUser user;
    std::string token;
};

class UserManager {
public:
    explicit UserManager(const std::string& filePath = "data/users.txt");
    explicit UserManager(DatabaseManager& database);

    AuthUser registerUser(const std::string& fullName, const std::string& username,
                          const std::string& password);
    AuthSession login(const std::string& username, const std::string& password);
    bool logout(const std::string& token);
    bool findSession(const std::string& token, AuthUser& user) const;

private:
    friend class DatabaseManager;

    struct UserRecord {
        AuthUser user;
        std::string salt;
        std::string passwordHash;
    };

    std::string filePath_;
    DatabaseManager* database_ = nullptr;
    std::map<std::string, UserRecord> users_;
    std::map<std::string, AuthUser> sessions_;

    void load();
    void save(const UserRecord& record) const;
    static std::map<std::string, UserRecord> readLegacyRecords(const std::string& filePath);
};
