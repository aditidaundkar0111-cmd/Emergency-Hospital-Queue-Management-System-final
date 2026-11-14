#include "DatabaseManager.h"

#include "Doctor.h"
#include "EmergencyPatient.h"
#include "FileManager.h"
#include "Hospital.h"
#include "Patient.h"
#include "RegularPatient.h"
#include "Treatment.h"
#include "UserManager.h"
#include <sqlite3.h>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace {
void checkSqlite(sqlite3* database, int result, const std::string& action) {
    if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW)
        throw std::runtime_error(action + ": " + sqlite3_errmsg(database));
}

void execute(sqlite3* database, const char* sql) {
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (result == SQLITE_OK) return;
    const std::string message = error != nullptr ? error : sqlite3_errmsg(database);
    sqlite3_free(error);
    throw std::runtime_error("SQLite operation failed: " + message);
}

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database), statement_(nullptr) {
        checkSqlite(database_, sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr),
                    "Could not prepare SQLite statement");
    }
    ~Statement() { if (statement_ != nullptr) sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const { return statement_; }
    void bindInt(int index, int value) {
        checkSqlite(database_, sqlite3_bind_int(statement_, index, value), "Could not bind SQLite integer");
    }
    void bindInt64(int index, sqlite3_int64 value) {
        checkSqlite(database_, sqlite3_bind_int64(statement_, index, value), "Could not bind SQLite integer");
    }
    void bindText(int index, const std::string& value) {
        checkSqlite(database_, sqlite3_bind_text(statement_, index, value.c_str(),
                                                static_cast<int>(value.size()), SQLITE_TRANSIENT),
                    "Could not bind SQLite text");
    }
    void bindNull(int index) {
        checkSqlite(database_, sqlite3_bind_null(statement_, index), "Could not bind SQLite NULL");
    }
    bool stepRow() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        checkSqlite(database_, result, "Could not read SQLite row");
        return false;
    }
    void stepDone() {
        const int result = sqlite3_step(statement_);
        checkSqlite(database_, result, "Could not write SQLite row");
    }
    void reset() {
        checkSqlite(database_, sqlite3_reset(statement_), "Could not reset SQLite statement");
        checkSqlite(database_, sqlite3_clear_bindings(statement_), "Could not clear SQLite statement");
    }

private:
    sqlite3* database_;
    sqlite3_stmt* statement_;
};

class Transaction {
public:
    explicit Transaction(sqlite3* database) : database_(database), committed_(false) {
        execute(database_, "BEGIN IMMEDIATE TRANSACTION");
    }
    ~Transaction() {
        if (!committed_) sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        execute(database_, "COMMIT");
        committed_ = true;
    }

private:
    sqlite3* database_;
    bool committed_;
};

std::string columnText(sqlite3_stmt* statement, int column) {
    const unsigned char* value = sqlite3_column_text(statement, column);
    return value == nullptr ? std::string() : reinterpret_cast<const char*>(value);
}

PatientStatus parseStatus(const std::string& status) {
    if (status == "WAITING") return PatientStatus::WAITING;
    if (status == "CALLED") return PatientStatus::CALLED;
    if (status == "ASSIGNED") return PatientStatus::ASSIGNED;
    if (status == "UNDER_TREATMENT") return PatientStatus::UNDER_TREATMENT;
    if (status == "COMPLETED") return PatientStatus::COMPLETED;
    throw std::runtime_error("Database contains an unknown patient status.");
}

std::string patientKind(const Patient& patient) {
    if (dynamic_cast<const EmergencyPatient*>(&patient) != nullptr) return "EMERGENCY";
    if (dynamic_cast<const RegularPatient*>(&patient) != nullptr) return "REGULAR";
    return "BASE";
}

long long epochSeconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
}

std::size_t tableCount(sqlite3* database, const char* table) {
    const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    Statement statement(database, sql.c_str());
    if (!statement.stepRow()) throw std::runtime_error("SQLite count query returned no row.");
    return static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));
}

void deleteRowsNotIn(sqlite3* database, const char* table, const char* column,
                     const std::vector<int>& retainedIds) {
    if (retainedIds.empty()) {
        execute(database, (std::string("DELETE FROM ") + table).c_str());
        return;
    }
    std::string sql = std::string("DELETE FROM ") + table + " WHERE " + column + " NOT IN (";
    for (std::size_t index = 0; index < retainedIds.size(); ++index) {
        if (index != 0) sql += ",";
        sql += "?";
    }
    sql += ")";
    Statement deletion(database, sql.c_str());
    for (std::size_t index = 0; index < retainedIds.size(); ++index)
        deletion.bindInt(static_cast<int>(index + 1), retainedIds[index]);
    deletion.stepDone();
}

int databaseUserVersion(sqlite3* database) {
    Statement version(database, "PRAGMA user_version");
    if (!version.stepRow()) throw std::runtime_error("Could not read database schema version.");
    return sqlite3_column_int(version.get(), 0);
}

bool hasApplicationSchemaObjects(sqlite3* database) {
    Statement objects(database,
        "SELECT 1 FROM sqlite_master WHERE type IN ('table','view','trigger') "
        "AND name NOT LIKE 'sqlite_%' LIMIT 1");
    return objects.stepRow();
}

void requireColumns(sqlite3* database, const char* table,
                    const std::set<std::string>& required) {
    const std::string sql = std::string("PRAGMA table_info('") + table + "')";
    Statement columns(database, sql.c_str());
    std::set<std::string> found;
    while (columns.stepRow()) found.insert(columnText(columns.get(), 1));
    if (found.empty()) throw std::runtime_error(std::string("Database schema is missing required table ") + table + ".");
    for (const std::string& column : required)
        if (found.find(column) == found.end())
            throw std::runtime_error(std::string("Database schema table ") + table +
                                     " is missing required column " + column + ".");
}

void requireNotNullColumns(sqlite3* database, const char* table,
                           const std::set<std::string>& required) {
    const std::string sql = std::string("PRAGMA table_info('") + table + "')";
    Statement columns(database, sql.c_str());
    while (columns.stepRow()) {
        const std::string name = columnText(columns.get(), 1);
        if (required.find(name) != required.end() && sqlite3_column_int(columns.get(), 3) == 0)
            throw std::runtime_error(std::string("Database schema column ") + table + "." + name +
                                     " is missing its NOT NULL constraint.");
    }
}

void requirePrimaryKey(sqlite3* database, const char* table, const char* column) {
    const std::string sql = std::string("PRAGMA table_info('") + table + "')";
    Statement columns(database, sql.c_str());
    while (columns.stepRow())
        if (columnText(columns.get(), 1) == column && sqlite3_column_int(columns.get(), 5) == 1)
            return;
    throw std::runtime_error(std::string("Database schema is missing primary key ") + table + "." + column + ".");
}

std::vector<std::string> indexColumns(sqlite3* database, const std::string& indexName) {
    Statement columns(database, (std::string("PRAGMA index_info('") + indexName + "')").c_str());
    std::vector<std::string> result;
    while (columns.stepRow()) result.push_back(columnText(columns.get(), 2));
    return result;
}

std::string compactSql(const std::string& sql);

void requireIndex(sqlite3* database, const char* table, const char* indexName,
                  const char* column, bool unique) {
    const std::string sql = std::string("PRAGMA index_list('") + table + "')";
    Statement indexes(database, sql.c_str());
    while (indexes.stepRow()) {
        const std::string name = columnText(indexes.get(), 1);
        if (name == indexName && sqlite3_column_int(indexes.get(), 2) == (unique ? 1 : 0) &&
            sqlite3_column_int(indexes.get(), 4) == 0 &&
            indexColumns(database, name) == std::vector<std::string>{column})
            return;
    }
    throw std::runtime_error(std::string("Database schema is missing required ") +
        (unique ? "unique " : "") + "index " + indexName + ".");
}

void requirePartialUniqueIndex(sqlite3* database, const char* table, const char* indexName,
                               const char* column) {
    const std::string sql = std::string("PRAGMA index_list('") + table + "')";
    Statement indexes(database, sql.c_str());
    while (indexes.stepRow()) {
        const std::string name = columnText(indexes.get(), 1);
        if (name == indexName && sqlite3_column_int(indexes.get(), 2) == 1 &&
            sqlite3_column_int(indexes.get(), 4) == 1 &&
            indexColumns(database, name) == std::vector<std::string>{column}) {
            Statement definition(database,
                "SELECT sql FROM sqlite_master WHERE type='index' AND name=?");
            definition.bindText(1, name);
            if (definition.stepRow() && compactSql(columnText(definition.get(), 0)).find("WHERESTATUS=ACTIVE") != std::string::npos)
                return;
        }
    }
    throw std::runtime_error(std::string("Database schema is missing required partial unique index ") + indexName + ".");
}

void requireUniqueColumn(sqlite3* database, const char* table, const char* column) {
    const std::string sql = std::string("PRAGMA index_list('") + table + "')";
    Statement indexes(database, sql.c_str());
    while (indexes.stepRow()) {
        const std::string name = columnText(indexes.get(), 1);
        if (sqlite3_column_int(indexes.get(), 2) == 1 && sqlite3_column_int(indexes.get(), 4) == 0 &&
            indexColumns(database, name) == std::vector<std::string>{column}) return;
    }
    throw std::runtime_error(std::string("Database schema is missing a unique constraint on ") +
                             table + "." + column + ".");
}

std::string compactSql(const std::string& sql) {
    std::string result;
    for (unsigned char character : sql) {
        if (std::isspace(character) || character == '\'' || character == '"' ||
            character == '`' || character == '[' || character == ']') continue;
        result.push_back(static_cast<char>(std::toupper(character)));
    }
    return result;
}

void requireChecks(sqlite3* database, const char* table,
                   const std::vector<std::string>& expressions) {
    Statement query(database, "SELECT sql FROM sqlite_master WHERE type='table' AND name=?");
    query.bindText(1, table);
    if (!query.stepRow()) throw std::runtime_error(std::string("Database schema is missing table ") + table + ".");
    const std::string definition = compactSql(columnText(query.get(), 0));
    for (const std::string& expression : expressions)
        if (definition.find(compactSql(expression)) == std::string::npos)
            throw std::runtime_error(std::string("Database schema table ") + table +
                                     " is missing a required CHECK constraint.");
}

void requireDefinitionFragments(sqlite3* database, const char* table,
                                const std::vector<std::string>& fragments) {
    Statement query(database, "SELECT sql FROM sqlite_master WHERE type='table' AND name=?");
    query.bindText(1, table);
    if (!query.stepRow()) throw std::runtime_error(std::string("Database schema is missing table ") + table + ".");
    const std::string definition = compactSql(columnText(query.get(), 0));
    for (const std::string& fragment : fragments)
        if (definition.find(compactSql(fragment)) == std::string::npos)
            throw std::runtime_error(std::string("Database schema table ") + table +
                                     " is missing a required column constraint.");
}

void requireForeignKey(sqlite3* database, const char* table, const char* referencedTable,
                       const char* fromColumn, const char* toColumn) {
    const std::string sql = std::string("PRAGMA foreign_key_list('") + table + "')";
    Statement keys(database, sql.c_str());
    while (keys.stepRow()) {
        if (columnText(keys.get(), 2) == referencedTable &&
            columnText(keys.get(), 3) == fromColumn && columnText(keys.get(), 4) == toColumn)
            return;
    }
    throw std::runtime_error(std::string("Database schema is missing required foreign key ") +
        table + "." + fromColumn + " -> " + referencedTable + "." + toColumn + ".");
}

void validateVersionOneSchema(sqlite3* database, bool includeAssignments = false) {
    Statement tables(database,
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'");
    std::set<std::string> foundTables;
    while (tables.stepRow()) foundTables.insert(columnText(tables.get(), 0));
    std::set<std::string> expectedTables = {"Patients", "Doctors", "Treatments", "Users", "Metadata"};
    if (includeAssignments) expectedTables.insert("Assignments");
    if (foundTables != expectedTables)
        throw std::runtime_error("Database contains an unfamiliar or incomplete application table set.");
    requireColumns(database, "Patients", {"id", "kind", "name", "age", "condition", "severity",
        "waiting_minutes", "critical", "status", "waiting_started_epoch_seconds"});
    requireColumns(database, "Doctors", {"id", "name", "specialization", "available", "current_patient_id"});
    requireColumns(database, "Treatments", {"patient_id", "doctor_id", "diagnosis", "description", "completed"});
    requireColumns(database, "Users", {"username_key", "username", "full_name", "salt", "password_hash"});
    requireColumns(database, "Metadata", {"key", "value"});
    requireNotNullColumns(database, "Patients", {"kind", "name", "age", "condition", "severity",
        "waiting_minutes", "critical", "status"});
    requireNotNullColumns(database, "Doctors", {"name", "specialization", "available"});
    requireNotNullColumns(database, "Treatments", {"doctor_id", "diagnosis", "description", "completed"});
    requireNotNullColumns(database, "Users", {"username", "full_name", "salt", "password_hash"});
    requireNotNullColumns(database, "Metadata", {"value"});
    requirePrimaryKey(database, "Patients", "id");
    requirePrimaryKey(database, "Doctors", "id");
    requirePrimaryKey(database, "Treatments", "patient_id");
    requirePrimaryKey(database, "Users", "username_key");
    requirePrimaryKey(database, "Metadata", "key");
    requireIndex(database, "Patients", "idx_patients_status", "status", false);
    requireIndex(database, "Doctors", "idx_doctors_availability", "available", false);
    requireUniqueColumn(database, "Doctors", "current_patient_id");
    requireUniqueColumn(database, "Users", "username");
    requireChecks(database, "Patients", {
        "CHECK(id > 0)", "CHECK(kind IN ('BASE','REGULAR','EMERGENCY'))",
        "CHECK(length(trim(name)) > 0)", "CHECK(age BETWEEN 0 AND 130)",
        "CHECK(length(trim(condition)) > 0)", "CHECK(severity BETWEEN 1 AND 5)",
        "CHECK(waiting_minutes >= 0)", "CHECK(critical IN (0,1))",
        "CHECK(status IN ('WAITING','CALLED','ASSIGNED','UNDER_TREATMENT','COMPLETED'))",
        "CHECK(kind = 'EMERGENCY' OR critical = 0)",
        "CHECK((status = 'WAITING' AND waiting_started_epoch_seconds IS NOT NULL) OR "
        "(status <> 'WAITING' AND waiting_started_epoch_seconds IS NULL))"});
    requireChecks(database, "Doctors", {
        "CHECK(id > 0)", "CHECK(length(trim(name)) > 0)",
        "CHECK(length(trim(specialization)) > 0)", "CHECK(available IN (0,1))",
        "CHECK((available = 1 AND current_patient_id IS NULL) OR "
        "(available = 0 AND current_patient_id IS NOT NULL))"});
    requireChecks(database, "Treatments", {
        "CHECK(length(trim(diagnosis)) > 0)", "CHECK(length(trim(description)) > 0)",
        "CHECK(completed IN (0,1))"});
    requireChecks(database, "Users", {"CHECK(length(trim(full_name)) > 0)"});
    requireDefinitionFragments(database, "Users", {"username TEXT NOT NULL UNIQUE COLLATE NOCASE"});
    requireForeignKey(database, "Doctors", "Patients", "current_patient_id", "id");
    requireForeignKey(database, "Treatments", "Patients", "patient_id", "id");
    requireForeignKey(database, "Treatments", "Doctors", "doctor_id", "id");
    if (includeAssignments) {
        requireColumns(database, "Assignments", {"id", "patient_id", "doctor_id",
            "assigned_at_epoch_seconds", "status", "completed_at_epoch_seconds", "timestamp_inferred"});
        requireNotNullColumns(database, "Assignments", {"patient_id", "doctor_id",
            "assigned_at_epoch_seconds", "status", "timestamp_inferred"});
        requirePrimaryKey(database, "Assignments", "id");
        requireIndex(database, "Assignments", "idx_assignments_patient", "patient_id", false);
        requireIndex(database, "Assignments", "idx_assignments_doctor", "doctor_id", false);
        requirePartialUniqueIndex(database, "Assignments", "uq_assignments_active_patient", "patient_id");
        requirePartialUniqueIndex(database, "Assignments", "uq_assignments_active_doctor", "doctor_id");
        requireChecks(database, "Assignments", {
            "CHECK(patient_id > 0)", "CHECK(doctor_id > 0)",
            "CHECK(assigned_at_epoch_seconds > 0)",
            "CHECK(status IN ('ACTIVE','COMPLETED'))",
            "CHECK(timestamp_inferred IN (0,1))",
            "CHECK((status = 'ACTIVE' AND completed_at_epoch_seconds IS NULL) OR "
            "(status = 'COMPLETED' AND completed_at_epoch_seconds IS NOT NULL))",
            "CHECK(completed_at_epoch_seconds IS NULL OR completed_at_epoch_seconds >= assigned_at_epoch_seconds)"});
        requireForeignKey(database, "Assignments", "Patients", "patient_id", "id");
        requireForeignKey(database, "Assignments", "Doctors", "doctor_id", "id");
    }
    Statement violations(database, "PRAGMA foreign_key_check");
    if (violations.stepRow()) throw std::runtime_error("Database contains rows that violate foreign-key constraints.");
}
}

DatabaseManager::DatabaseManager(const std::string& databasePath)
    : database_(nullptr), databasePath_(databasePath) {
    if (databasePath_.empty()) throw std::invalid_argument("Database path cannot be empty.");
    const std::filesystem::path parent = std::filesystem::path(databasePath_).parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) throw std::runtime_error("Could not create database directory: " + error.message());
    }
    const int result = sqlite3_open_v2(databasePath_.c_str(), &database_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (result != SQLITE_OK) {
        const std::string message = database_ != nullptr ? sqlite3_errmsg(database_) : "allocation failed";
        if (database_ != nullptr) sqlite3_close_v2(database_);
        database_ = nullptr;
        throw std::runtime_error("Could not open SQLite database '" + databasePath_ + "': " + message);
    }
    try {
        sqlite3_busy_timeout(database_, 5000);
        execute(database_, "PRAGMA foreign_keys = ON");
        createSchema();
        execute(database_, "PRAGMA journal_mode = WAL");
        execute(database_, "PRAGMA synchronous = FULL");
    } catch (...) {
        sqlite3_close_v2(database_);
        database_ = nullptr;
        throw;
    }
}

DatabaseManager::~DatabaseManager() {
    if (database_ != nullptr) sqlite3_close_v2(database_);
}

void DatabaseManager::createSchema() {
    const int schemaVersion = databaseUserVersion(database_);
    if (schemaVersion < 0 || schemaVersion > 2)
        throw std::runtime_error("Database schema version is not supported.");
    if (schemaVersion == 2) {
        validateVersionTwoSchema();
        return;
    }
    if (schemaVersion == 1) {
        validateVersionOneSchema(database_);
        migrateSchemaVersionOneToTwo();
        return;
    }
    if (hasApplicationSchemaObjects(database_))
        throw std::runtime_error("Unversioned database contains existing application objects; refusing to adopt it.");

    Transaction transaction(database_);
    execute(database_,
        "CREATE TABLE Patients ("
        "id INTEGER PRIMARY KEY CHECK(id > 0),"
        "kind TEXT NOT NULL CHECK(kind IN ('BASE','REGULAR','EMERGENCY')),"
        "name TEXT NOT NULL CHECK(length(trim(name)) > 0),"
        "age INTEGER NOT NULL CHECK(age BETWEEN 0 AND 130),"
        "condition TEXT NOT NULL CHECK(length(trim(condition)) > 0),"
        "severity INTEGER NOT NULL CHECK(severity BETWEEN 1 AND 5),"
        "waiting_minutes INTEGER NOT NULL CHECK(waiting_minutes >= 0),"
        "critical INTEGER NOT NULL CHECK(critical IN (0,1)),"
        "status TEXT NOT NULL CHECK(status IN ('WAITING','CALLED','ASSIGNED','UNDER_TREATMENT','COMPLETED'))," 
        "waiting_started_epoch_seconds INTEGER," 
        "CHECK(kind = 'EMERGENCY' OR critical = 0),"
        "CHECK((status = 'WAITING' AND waiting_started_epoch_seconds IS NOT NULL) OR "
        "(status <> 'WAITING' AND waiting_started_epoch_seconds IS NULL))"
        ");"
        "CREATE TABLE Doctors ("
        "id INTEGER PRIMARY KEY CHECK(id > 0),"
        "name TEXT NOT NULL CHECK(length(trim(name)) > 0),"
        "specialization TEXT NOT NULL CHECK(length(trim(specialization)) > 0),"
        "available INTEGER NOT NULL CHECK(available IN (0,1)),"
        "current_patient_id INTEGER UNIQUE REFERENCES Patients(id) ON DELETE RESTRICT,"
        "CHECK((available = 1 AND current_patient_id IS NULL) OR "
        "(available = 0 AND current_patient_id IS NOT NULL))"
        ");"
        "CREATE TABLE Treatments ("
        "patient_id INTEGER PRIMARY KEY REFERENCES Patients(id) ON DELETE RESTRICT,"
        "doctor_id INTEGER NOT NULL REFERENCES Doctors(id) ON DELETE RESTRICT,"
        "diagnosis TEXT NOT NULL CHECK(length(trim(diagnosis)) > 0),"
        "description TEXT NOT NULL CHECK(length(trim(description)) > 0),"
        "completed INTEGER NOT NULL CHECK(completed IN (0,1))"
        ");"
        "CREATE TABLE Users ("
        "username_key TEXT PRIMARY KEY COLLATE NOCASE,"
        "username TEXT NOT NULL UNIQUE COLLATE NOCASE,"
        "full_name TEXT NOT NULL CHECK(length(trim(full_name)) > 0),"
        "salt TEXT NOT NULL,"
        "password_hash TEXT NOT NULL"
        ");"
        "CREATE TABLE Metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE INDEX idx_patients_status ON Patients(status);"
        "CREATE INDEX idx_doctors_availability ON Doctors(available);");
    execute(database_, "PRAGMA user_version = 1");
    validateVersionOneSchema(database_);
    transaction.commit();
    migrateSchemaVersionOneToTwo();
}

void DatabaseManager::validateVersionTwoSchema() const {
    validateVersionOneSchema(database_, true);
}

void DatabaseManager::migrateSchemaVersionOneToTwo() {
    Transaction transaction(database_);
    const std::int64_t migrationTime = epochSeconds(std::chrono::system_clock::now());
    execute(database_,
        "CREATE TABLE Assignments("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "patient_id INTEGER NOT NULL REFERENCES Patients(id) ON DELETE CASCADE CHECK(patient_id > 0),"
        "doctor_id INTEGER NOT NULL REFERENCES Doctors(id) ON DELETE CASCADE CHECK(doctor_id > 0),"
        "assigned_at_epoch_seconds INTEGER NOT NULL CHECK(assigned_at_epoch_seconds > 0),"
        "status TEXT NOT NULL CHECK(status IN ('ACTIVE','COMPLETED'))," 
        "completed_at_epoch_seconds INTEGER," 
        "timestamp_inferred INTEGER NOT NULL CHECK(timestamp_inferred IN (0,1)),"
        "CHECK((status = 'ACTIVE' AND completed_at_epoch_seconds IS NULL) OR "
        "(status = 'COMPLETED' AND completed_at_epoch_seconds IS NOT NULL)),"
        "CHECK(completed_at_epoch_seconds IS NULL OR completed_at_epoch_seconds >= assigned_at_epoch_seconds)"
        ");"
        "CREATE INDEX idx_assignments_patient ON Assignments(patient_id);"
        "CREATE INDEX idx_assignments_doctor ON Assignments(doctor_id);"
        "CREATE UNIQUE INDEX uq_assignments_active_patient ON Assignments(patient_id) WHERE status='ACTIVE';"
        "CREATE UNIQUE INDEX uq_assignments_active_doctor ON Assignments(doctor_id) WHERE status='ACTIVE';");
    {
        Statement completed(database_,
            "INSERT INTO Assignments(patient_id,doctor_id,assigned_at_epoch_seconds,status,"
            "completed_at_epoch_seconds,timestamp_inferred) "
            "SELECT t.patient_id,t.doctor_id,?,'COMPLETED',?,1 FROM Treatments t "
            "JOIN Patients p ON p.id=t.patient_id WHERE t.completed=1 AND p.status='COMPLETED'");
        completed.bindInt64(1, migrationTime);
        completed.bindInt64(2, migrationTime);
        completed.stepDone();
    }
    {
        Statement active(database_,
            "INSERT INTO Assignments(patient_id,doctor_id,assigned_at_epoch_seconds,status,"
            "completed_at_epoch_seconds,timestamp_inferred) "
            "SELECT d.current_patient_id,d.id,?,'ACTIVE',NULL,1 FROM Doctors d "
            "JOIN Patients p ON p.id=d.current_patient_id "
            "WHERE d.current_patient_id IS NOT NULL AND p.status IN ('ASSIGNED','UNDER_TREATMENT')");
        active.bindInt64(1, migrationTime);
        active.stepDone();
    }
    {
        Statement inconsistencies(database_,
            "SELECT 1 FROM Doctors d LEFT JOIN Patients p ON p.id=d.current_patient_id "
            "WHERE d.available=0 AND (p.id IS NULL OR p.status NOT IN ('ASSIGNED','UNDER_TREATMENT')) "
            "UNION ALL SELECT 1 FROM Patients p WHERE p.status IN ('ASSIGNED','UNDER_TREATMENT') "
            "AND NOT EXISTS(SELECT 1 FROM Doctors d WHERE d.available=0 AND d.current_patient_id=p.id) "
            "UNION ALL SELECT 1 FROM Treatments t JOIN Patients p ON p.id=t.patient_id "
            "WHERE t.completed=1 AND p.status<>'COMPLETED' LIMIT 1");
        if (inconsistencies.stepRow())
            throw std::runtime_error("Existing database lifecycle relationships are inconsistent; assignment migration was rolled back.");
    }
    execute(database_, "PRAGMA user_version = 2");
    validateVersionTwoSchema();
    transaction.commit();
}

DatabaseMigrationSummary DatabaseManager::initializeAndLoad(
    Hospital& hospital, const std::string& dataDirectory, const std::string& userFile) {
    DatabaseMigrationSummary summary;
    Transaction transaction(database_);
    bool migratedAlready = false;
    {
        Statement marker(database_, "SELECT value FROM Metadata WHERE key = 'text_migration_v1'");
        migratedAlready = marker.stepRow();
    }
    if (migratedAlready) {
        transaction.commit();
        loadHospital(hospital);
        return summary;
    }
    if (tableCount(database_, "Patients") != 0 || tableCount(database_, "Doctors") != 0 ||
        tableCount(database_, "Treatments") != 0 || tableCount(database_, "Users") != 0)
        throw std::runtime_error("Database has records but no migration marker; refusing to merge legacy data.");

    Hospital imported;
    FileManager(dataDirectory).loadAllReadOnly(imported);
    const std::map<std::string, UserManager::UserRecord> legacyUsers =
        UserManager::readLegacyRecords(userFile);

    replaceHospitalRows(imported);
    Statement insert(database_,
        "INSERT INTO Users(username_key,username,full_name,salt,password_hash) VALUES(?,?,?,?,?)");
    for (const auto& entry : legacyUsers) {
        insert.bindText(1, entry.first);
        insert.bindText(2, entry.second.user.username);
        insert.bindText(3, entry.second.user.fullName);
        insert.bindText(4, entry.second.salt);
        insert.bindText(5, entry.second.passwordHash);
        insert.stepDone();
        insert.reset();
    }

    Hospital validated;
    loadHospitalRows(validated);
    execute(database_, "INSERT INTO Metadata(key,value) VALUES('text_migration_v1','complete')");
    transaction.commit();

    summary.migrated = true;
    summary.patients = imported.getPatients().size();
    summary.doctors = imported.getDoctors().size();
    summary.treatments = imported.getTreatments().size();
    summary.users = legacyUsers.size();
    loadHospital(hospital);
    return summary;
}

void DatabaseManager::initializeEmptyDatabase(Hospital& hospital, const std::string& dataDirectory,
                                              const std::string& userFile) {
    const char* dataNames[] = {"patients.txt", "doctors.txt", "treatments.txt"};
    for (const char* name : dataNames) {
        std::error_code error;
        if (std::filesystem::exists(std::filesystem::path(dataDirectory) / name, error))
            throw std::runtime_error("Cannot initialize an empty database while legacy data exists: " +
                (std::filesystem::path(dataDirectory) / name).string() + ". Use normal migration.");
        if (error) throw std::runtime_error("Could not inspect legacy data path: " + error.message());
    }
    {
        std::error_code error;
        if (std::filesystem::exists(userFile, error))
            throw std::runtime_error("Cannot initialize an empty database while a legacy account file exists. Use normal migration.");
        if (error) throw std::runtime_error("Could not inspect legacy account path: " + error.message());
    }
    Transaction transaction(database_);
    {
        Statement marker(database_, "SELECT value FROM Metadata WHERE key = 'text_migration_v1'");
        if (marker.stepRow())
            throw std::runtime_error("Database is already initialized; empty initialization is only for a new installation.");
    }
    if (tableCount(database_, "Patients") != 0 || tableCount(database_, "Doctors") != 0 ||
        tableCount(database_, "Treatments") != 0 || tableCount(database_, "Users") != 0)
        throw std::runtime_error("Database already contains records; refusing empty initialization.");
    execute(database_, "INSERT INTO Metadata(key,value) VALUES('text_migration_v1','empty-initialization')");
    transaction.commit();
    loadHospital(hospital);
}

void DatabaseManager::replaceHospitalRows(const Hospital& hospital) const {
    // Break the Doctor -> Patient current-assignment reference before applying a
    // complete in-memory snapshot. Assignment history itself is retained.
    execute(database_, "UPDATE Doctors SET available=1,current_patient_id=NULL");

    Statement patientInsert(database_,
        "INSERT INTO Patients(id,kind,name,age,condition,severity,waiting_minutes,critical,status,waiting_started_epoch_seconds) "
        "VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
        "kind=excluded.kind,name=excluded.name,age=excluded.age,condition=excluded.condition,"
        "severity=excluded.severity,waiting_minutes=excluded.waiting_minutes,critical=excluded.critical,"
        "status=excluded.status,waiting_started_epoch_seconds=excluded.waiting_started_epoch_seconds");
    std::vector<int> patientIds;
    for (const auto& owned : hospital.getPatients()) {
        const Patient& patient = *owned;
        patientIds.push_back(patient.getId());
        const auto* emergency = dynamic_cast<const EmergencyPatient*>(&patient);
        patientInsert.bindInt(1, patient.getId());
        patientInsert.bindText(2, patientKind(patient));
        patientInsert.bindText(3, patient.getName());
        patientInsert.bindInt(4, patient.getAge());
        patientInsert.bindText(5, patient.getCondition());
        patientInsert.bindInt(6, patient.getSeverity());
        patientInsert.bindInt(7, patient.waitingMinutes_);
        patientInsert.bindInt(8, emergency != nullptr && emergency->isCritical() ? 1 : 0);
        patientInsert.bindText(9, toString(patient.getStatus()));
        if (patient.getStatus() == PatientStatus::WAITING)
            patientInsert.bindInt64(10, epochSeconds(patient.waitingStartedAt_));
        else patientInsert.bindNull(10);
        patientInsert.stepDone();
        patientInsert.reset();
    }

    Statement doctorInsert(database_,
        "INSERT INTO Doctors(id,name,specialization,available,current_patient_id) VALUES(?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name,specialization=excluded.specialization,"
        "available=excluded.available,current_patient_id=excluded.current_patient_id");
    std::vector<int> doctorIds;
    for (const Doctor& doctor : hospital.getDoctors()) {
        doctorIds.push_back(doctor.getDoctorId());
        doctorInsert.bindInt(1, doctor.getDoctorId());
        doctorInsert.bindText(2, doctor.getName());
        doctorInsert.bindText(3, doctor.getSpecialization());
        doctorInsert.bindInt(4, doctor.isAvailable() ? 1 : 0);
        if (doctor.isAvailable()) doctorInsert.bindNull(5);
        else doctorInsert.bindInt(5, doctor.getCurrentPatientId());
        doctorInsert.stepDone();
        doctorInsert.reset();
    }

    Statement treatmentInsert(database_,
        "INSERT INTO Treatments(patient_id,doctor_id,diagnosis,description,completed) VALUES(?,?,?,?,?) "
        "ON CONFLICT(patient_id) DO UPDATE SET doctor_id=excluded.doctor_id,diagnosis=excluded.diagnosis,"
        "description=excluded.description,completed=excluded.completed");
    std::vector<int> treatmentPatientIds;
    for (const Treatment& treatment : hospital.getTreatments()) {
        treatmentPatientIds.push_back(treatment.getPatientId());
        treatmentInsert.bindInt(1, treatment.getPatientId());
        treatmentInsert.bindInt(2, treatment.getDoctorId());
        treatmentInsert.bindText(3, treatment.getDiagnosis());
        treatmentInsert.bindText(4, treatment.getDescription());
        treatmentInsert.bindInt(5, treatment.isCompleted() ? 1 : 0);
        treatmentInsert.stepDone();
        treatmentInsert.reset();
    }

    synchronizeAssignments(hospital);
    deleteRowsNotIn(database_, "Treatments", "patient_id", treatmentPatientIds);
    deleteRowsNotIn(database_, "Doctors", "id", doctorIds);
    deleteRowsNotIn(database_, "Patients", "id", patientIds);
}

void DatabaseManager::synchronizeAssignments(const Hospital& hospital) const {
    const std::int64_t now = epochSeconds(std::chrono::system_clock::now());
    Statement findActive(database_,
        "SELECT id,doctor_id FROM Assignments WHERE patient_id=? AND status='ACTIVE'");
    Statement insert(database_,
        "INSERT INTO Assignments(patient_id,doctor_id,assigned_at_epoch_seconds,status,"
        "completed_at_epoch_seconds,timestamp_inferred) VALUES(?,?,?,'ACTIVE',NULL,0)");
    for (const Doctor& doctor : hospital.getDoctors()) {
        if (doctor.isAvailable()) continue;
        const Patient* patient = hospital.findPatient(doctor.getCurrentPatientId());
        if (patient == nullptr || (patient->getStatus() != PatientStatus::ASSIGNED &&
                                   patient->getStatus() != PatientStatus::UNDER_TREATMENT))
            throw std::runtime_error("Cannot persist an assignment with inconsistent patient and doctor state.");
        findActive.bindInt(1, patient->getId());
        if (findActive.stepRow()) {
            if (sqlite3_column_int(findActive.get(), 1) != doctor.getDoctorId())
                throw std::runtime_error("A patient already has a different active doctor assignment.");
        } else {
            insert.bindInt(1, patient->getId());
            insert.bindInt(2, doctor.getDoctorId());
            insert.bindInt64(3, now);
            insert.stepDone();
            insert.reset();
        }
        findActive.reset();
    }

    Statement completedAssignment(database_,
        "SELECT id,doctor_id FROM Assignments WHERE patient_id=? AND status='ACTIVE'");
    Statement finish(database_,
        "UPDATE Assignments SET status='COMPLETED',completed_at_epoch_seconds=? WHERE id=? AND status='ACTIVE'");
    Statement insertCompleted(database_,
        "INSERT INTO Assignments(patient_id,doctor_id,assigned_at_epoch_seconds,status,"
        "completed_at_epoch_seconds,timestamp_inferred) VALUES(?,?,?,'COMPLETED',?,1)");
    Statement existsCompleted(database_,
        "SELECT 1 FROM Assignments WHERE patient_id=? AND doctor_id=? AND status='COMPLETED' LIMIT 1");
    for (const Treatment& treatment : hospital.getTreatments()) {
        if (!treatment.isCompleted()) continue;
        completedAssignment.bindInt(1, treatment.getPatientId());
        if (completedAssignment.stepRow()) {
            if (sqlite3_column_int(completedAssignment.get(), 1) != treatment.getDoctorId())
                throw std::runtime_error("Completed treatment does not match the active assignment.");
            finish.bindInt64(1, now);
            finish.bindInt(2, sqlite3_column_int(completedAssignment.get(), 0));
            finish.stepDone();
            finish.reset();
        } else {
            existsCompleted.bindInt(1, treatment.getPatientId());
            existsCompleted.bindInt(2, treatment.getDoctorId());
            if (!existsCompleted.stepRow()) {
                insertCompleted.bindInt(1, treatment.getPatientId());
                insertCompleted.bindInt(2, treatment.getDoctorId());
                insertCompleted.bindInt64(3, now);
                insertCompleted.bindInt64(4, now);
                insertCompleted.stepDone();
                insertCompleted.reset();
            }
            existsCompleted.reset();
        }
        completedAssignment.reset();
    }
}

void DatabaseManager::saveHospital(const Hospital& hospital) const {
    Transaction transaction(database_);
    replaceHospitalRows(hospital);
    Hospital validated;
    loadHospitalRows(validated);
    transaction.commit();
}

void DatabaseManager::loadHospitalRows(Hospital& hospital) const {
    Statement patients(database_,
        "SELECT id,kind,name,age,condition,severity,waiting_minutes,critical,status,waiting_started_epoch_seconds "
        "FROM Patients ORDER BY id");
    while (patients.stepRow()) {
        const int id = sqlite3_column_int(patients.get(), 0);
        const std::string kind = columnText(patients.get(), 1);
        const std::string name = columnText(patients.get(), 2);
        const int age = sqlite3_column_int(patients.get(), 3);
        const std::string condition = columnText(patients.get(), 4);
        const int severity = sqlite3_column_int(patients.get(), 5);
        const int waitingMinutes = sqlite3_column_int(patients.get(), 6);
        const bool critical = sqlite3_column_int(patients.get(), 7) != 0;
        const PatientStatus status = parseStatus(columnText(patients.get(), 8));
        std::unique_ptr<Patient> patient;
        if (kind == "EMERGENCY")
            patient.reset(new EmergencyPatient(id, name, age, condition, severity, critical, waitingMinutes));
        else if (kind == "REGULAR")
            patient.reset(new RegularPatient(id, name, age, condition, severity, waitingMinutes));
        else if (kind == "BASE")
            patient.reset(new Patient(id, name, age, condition, severity, waitingMinutes));
        else throw std::runtime_error("Database contains an unknown patient type.");

        hospital.restorePatient(std::move(patient), status);
        Patient* restored = hospital.findPatient(id);
        if (status == PatientStatus::WAITING) {
            if (sqlite3_column_type(patients.get(), 9) == SQLITE_NULL)
                throw std::runtime_error("Waiting patient has no saved arrival timestamp.");
            restored->setWaitingStartedAtEpochSeconds(sqlite3_column_int64(patients.get(), 9));
        }
    }

    Statement doctors(database_,
        "SELECT id,name,specialization,available,current_patient_id FROM Doctors ORDER BY id");
    while (doctors.stepRow()) {
        const bool available = sqlite3_column_int(doctors.get(), 3) != 0;
        const int currentPatientId = sqlite3_column_type(doctors.get(), 4) == SQLITE_NULL
            ? -1 : sqlite3_column_int(doctors.get(), 4);
        hospital.addDoctor(Doctor(sqlite3_column_int(doctors.get(), 0), columnText(doctors.get(), 1),
                                  columnText(doctors.get(), 2), available, currentPatientId));
    }

    for (const auto& patient : hospital.getPatients()) {
        if (patient->getStatus() != PatientStatus::ASSIGNED &&
            patient->getStatus() != PatientStatus::UNDER_TREATMENT) continue;
        bool matchedDoctor = false;
        for (const Doctor& doctor : hospital.getDoctors())
            if (!doctor.isAvailable() && doctor.getCurrentPatientId() == patient->getId())
                matchedDoctor = true;
        if (!matchedDoctor)
            throw std::runtime_error("Patient assignment is inconsistent with doctor records.");
    }
    for (const Doctor& doctor : hospital.getDoctors()) {
        if (!doctor.isAvailable()) {
            const Patient* patient = hospital.findPatient(doctor.getCurrentPatientId());
            if (patient == nullptr || (patient->getStatus() != PatientStatus::ASSIGNED &&
                                       patient->getStatus() != PatientStatus::UNDER_TREATMENT))
                throw std::runtime_error("Busy doctor assignment is inconsistent with patient status.");
        }
    }

    Statement treatments(database_,
        "SELECT patient_id,doctor_id,diagnosis,description,completed FROM Treatments ORDER BY patient_id");
    while (treatments.stepRow()) {
        hospital.restoreTreatment(Treatment(sqlite3_column_int(treatments.get(), 0),
            sqlite3_column_int(treatments.get(), 1), columnText(treatments.get(), 2),
            columnText(treatments.get(), 3), sqlite3_column_int(treatments.get(), 4) != 0));
    }
    hospital.rebuildWaitingQueue();
}

void DatabaseManager::loadHospital(Hospital& hospital) const {
    Hospital loaded;
    loadHospitalRows(loaded);
    hospital.replaceWith(std::move(loaded));
}

std::vector<AssignmentRecord> DatabaseManager::getAssignments() const {
    std::vector<AssignmentRecord> result;
    Statement assignments(database_,
        "SELECT id,patient_id,doctor_id,assigned_at_epoch_seconds,status,"
        "completed_at_epoch_seconds,timestamp_inferred FROM Assignments ORDER BY id");
    while (assignments.stepRow()) {
        AssignmentRecord record;
        record.id = sqlite3_column_int(assignments.get(), 0);
        record.patientId = sqlite3_column_int(assignments.get(), 1);
        record.doctorId = sqlite3_column_int(assignments.get(), 2);
        record.assignedAtEpochSeconds = sqlite3_column_int64(assignments.get(), 3);
        record.status = columnText(assignments.get(), 4);
        record.completedAtEpochSeconds = sqlite3_column_type(assignments.get(), 5) == SQLITE_NULL
            ? 0 : sqlite3_column_int64(assignments.get(), 5);
        record.timestampInferred = sqlite3_column_int(assignments.get(), 6) != 0;
        result.push_back(record);
    }
    return result;
}

std::vector<DatabaseManager::StoredUser> DatabaseManager::loadUsers() const {
    std::vector<StoredUser> result;
    Statement users(database_, "SELECT full_name,username,salt,password_hash FROM Users ORDER BY username_key");
    while (users.stepRow())
        result.push_back(StoredUser{columnText(users.get(), 0), columnText(users.get(), 1),
                                    columnText(users.get(), 2), columnText(users.get(), 3)});
    return result;
}

void DatabaseManager::insertUser(const std::string& normalizedUsername, const StoredUser& user) {
    Transaction transaction(database_);
    Statement insert(database_,
        "INSERT INTO Users(username_key,username,full_name,salt,password_hash) VALUES(?,?,?,?,?)");
    insert.bindText(1, normalizedUsername);
    insert.bindText(2, user.username);
    insert.bindText(3, user.fullName);
    insert.bindText(4, user.salt);
    insert.bindText(5, user.passwordHash);
    insert.stepDone();
    transaction.commit();
}
