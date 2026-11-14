#pragma once

#include "Hospital.h"
#include "UserManager.h"
#include "httplib.h"
#include <string>

class DatabaseManager;

class WebServer {
public:
    explicit WebServer(Hospital& hospital, DatabaseManager& database,
                       const std::string& webDirectory = "web");
    bool listen(const std::string& host = "127.0.0.1", int port = 8080);
    void stop();

private:
    Hospital& hospital_;
    DatabaseManager& database_;
    UserManager userManager_;
    httplib::Server server_;

    void registerRoutes();
    void persistHospitalOrRestore();
};
