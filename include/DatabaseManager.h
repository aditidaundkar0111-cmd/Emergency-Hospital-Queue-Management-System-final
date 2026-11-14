#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Hospital;
class UserManager;
struct sqlite3;

struct DatabaseMigrationSummary {
    bool migrated = false;
    std::size_t patients = 0;
    std::size_t doctors = 0;
    std::size_t treatments = 0;
    std::size_t users = 0;
};

struct AssignmentRecord {
    int id = 0;
    int patientId = 0;
    int doctorId = 0;
    std::int64_t assignedAtEpochSeconds = 0;
    std::string status;
    std::int64_t completedAtEpochSeconds = 0;
    bool timestampInferred = false;
};

class DatabaseManager {
public:
    explicit DatabaseManager(const std::string& databasePath = "hospital.db");
    ~DatabaseManager();

    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    DatabaseMigrationSummary initializeAndLoad(
        Hospital& hospital, const std::string& dataDirectory = "data",
        const std::string& userFile = "data/users.txt");
    void initializeEmptyDatabase(Hospital& hospital, const std::string& dataDirectory = "data",
                                 const std::string& userFile = "data/users.txt");
    void saveHospital(const Hospital& hospital) const;
    void loadHospital(Hospital& hospital) const;
    std::vector<AssignmentRecord> getAssignments() const;

private:
    friend class UserManager;

    struct StoredUser {
        std::string fullName;
        std::string username;
        std::string salt;
        std::string passwordHash;
    };

    sqlite3* database_;
    std::string databasePath_;

    void createSchema();
    void migrateSchemaVersionOneToTwo();
    void validateVersionTwoSchema() const;
    std::vector<StoredUser> loadUsers() const;
    void insertUser(const std::string& normalizedUsername, const StoredUser& user);
    void replaceHospitalRows(const Hospital& hospital) const;
    void synchronizeAssignments(const Hospital& hospital) const;
    void loadHospitalRows(Hospital& hospital) const;
};
