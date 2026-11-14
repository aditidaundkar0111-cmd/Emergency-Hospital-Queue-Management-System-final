#include "Hospital.h"
#include "DatabaseManager.h"
#include "ProjectPaths.h"
#include "WebServer.h"
#include <filesystem>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    try {
        Hospital hospital;
        const bool initializeEmpty = argc == 2 && std::string(argv[1]) == "--initialize-empty";
        if (argc > 2 || (argc == 2 && !initializeEmpty)) {
            std::cerr << "Usage: emergency_hospital_web.exe [--initialize-empty]\n";
            return 2;
        }
        const std::filesystem::path root = ProjectPaths::projectRoot(argv[0]);
        const std::filesystem::path dataDirectory = root / "data";
        DatabaseManager database((root / "hospital.db").string());
        if (initializeEmpty) {
            database.initializeEmptyDatabase(hospital, dataDirectory.string(),
                                             (dataDirectory / "users.txt").string());
            std::cout << "Initialized an empty SQLite database by explicit request.\n";
        } else {
            const DatabaseMigrationSummary migration = database.initializeAndLoad(
                hospital, dataDirectory.string(), (dataDirectory / "users.txt").string());
            if (migration.migrated)
                std::cout << "Migrated " << migration.patients << " patients, " << migration.doctors
                          << " doctors, " << migration.treatments << " treatments and "
                          << migration.users << " accounts into SQLite.\n";
        }
        WebServer server(hospital, database);
        std::cout << "Emergency Hospital API listening on http://localhost:8080\n";
        std::cout << "Server uses single-threaded request handling on the local machine.\n";
        std::cout << "Press Ctrl+C to stop the server.\n";
        if (!server.listen("127.0.0.1", 8080)) {
            std::cerr << "Could not start the web server on port 8080.\n";
            return 1;
        }
        return 0;
    } catch (const std::exception&) {
        std::cerr << "Web server startup failed. Check the database and startup configuration.\n";
        return 1;
    }
}
