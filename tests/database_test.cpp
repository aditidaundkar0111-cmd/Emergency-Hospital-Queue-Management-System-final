#include "DatabaseManager.h"
#include "EmergencyPatient.h"
#include "FileManager.h"
#include "Hospital.h"
#include "ProjectPaths.h"
#include "RegularPatient.h"
#include "UserManager.h"
#include <sqlite3.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace {
struct TemporaryDirectory {
    std::string path;
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string makeTemporaryRoot() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("ehqo_sqlite_tests_" + std::to_string(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    return root.string();
}

std::string readBytes(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (!input.good() && !input.eof()) throw std::runtime_error("Could not read test fixture.");
    return bytes.str();
}

void writeText(const std::string& path, const std::string& text) {
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    output << text;
    if (!output) throw std::runtime_error("Could not write isolated test fixture.");
}

std::size_t scalarCount(sqlite3* db, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const std::size_t count = static_cast<std::size_t>(sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement);
    return count;
}

void executeRaw(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    const std::string message = error == nullptr ? std::string() : error;
    sqlite3_free(error);
    if (result != SQLITE_OK) throw std::runtime_error("Could not prepare isolated schema fixture: " + message);
}

std::vector<std::string> tableSnapshot(sqlite3* db, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error("Could not read isolated migration snapshot.");
    std::vector<std::string> rows;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        std::string row;
        for (int column = 0; column < sqlite3_column_count(statement); ++column) {
            if (column != 0) row.push_back('\x1f');
            const unsigned char* value = sqlite3_column_text(statement, column);
            if (value != nullptr) row += reinterpret_cast<const char*>(value);
        }
        rows.push_back(row);
    }
    sqlite3_finalize(statement);
    if (step != SQLITE_DONE) throw std::runtime_error("Could not finish isolated migration snapshot.");
    return rows;
}

struct CoreSnapshot {
    int schemaVersion = 0;
    std::vector<std::string> patients;
    std::vector<std::string> doctors;
    std::vector<std::string> treatments;
    std::size_t userCount = 0;
};

CoreSnapshot readCoreSnapshot(const std::string& path) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("Could not open isolated database snapshot read-only.");
    CoreSnapshot snapshot;
    sqlite3_stmt* version = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &version, nullptr) != SQLITE_OK ||
        sqlite3_step(version) != SQLITE_ROW) {
        sqlite3_finalize(version);
        sqlite3_close(database);
        throw std::runtime_error("Could not read isolated database version.");
    }
    snapshot.schemaVersion = sqlite3_column_int(version, 0);
    sqlite3_finalize(version);
    snapshot.patients = tableSnapshot(database,
        "SELECT id,kind,name,age,condition,severity,waiting_minutes,critical,status,waiting_started_epoch_seconds "
        "FROM Patients ORDER BY id");
    snapshot.doctors = tableSnapshot(database,
        "SELECT id,name,specialization,available,current_patient_id FROM Doctors ORDER BY id");
    snapshot.treatments = tableSnapshot(database,
        "SELECT patient_id,doctor_id,diagnosis,description,completed FROM Treatments ORDER BY patient_id");
    snapshot.userCount = scalarCount(database, "SELECT COUNT(*) FROM Users");
    sqlite3_close(database);
    return snapshot;
}

void createEmptyLegacyData(const std::string& directory) {
    std::filesystem::create_directories(directory);
    writeText(directory + "/patients.txt",
        "# P id type age severity wait critical status \"name\" \"condition\" [timestamp]\n");
    writeText(directory + "/doctors.txt",
        "# D id available currentPatientId \"name\" \"specialization\"\n");
    writeText(directory + "/treatments.txt",
        "# T patientId doctorId completed \"diagnosis\" \"description\"\n");
}

void assertFailedMigrationLeavesNoMarker(const std::string& root, const std::string& dataDirectory,
                                         const std::string& usersFile) {
    const std::string databasePath = root + "/hospital.db";
    DatabaseManager database(databasePath);
    Hospital hospital;
    bool rejected = false;
    try { database.initializeAndLoad(hospital, dataDirectory, usersFile); }
    catch (const std::exception&) { rejected = true; }
    assert(rejected);
    sqlite3* raw = nullptr;
    assert(sqlite3_open(databasePath.c_str(), &raw) == SQLITE_OK);
    assert(scalarCount(raw, "SELECT COUNT(*) FROM Metadata WHERE key='text_migration_v1'") == 0);
    assert(scalarCount(raw, "SELECT COUNT(*) FROM Patients") == 0);
    sqlite3_close(raw);
}

void testMigrationSafety(const std::string& root) {
    {
        const std::string caseRoot = root + "/missing_data_directory";
        std::filesystem::create_directories(caseRoot);
        assertFailedMigrationLeavesNoMarker(caseRoot, caseRoot + "/data", caseRoot + "/missing-users.txt");
    }
    const char* missingNames[] = {"patients.txt", "doctors.txt", "treatments.txt"};
    for (const char* missing : missingNames) {
        const std::string caseRoot = root + "/missing_" + missing;
        const std::string data = caseRoot + "/data";
        createEmptyLegacyData(data);
        std::filesystem::remove(data + "/" + missing);
        const std::string dbPath = caseRoot + "/hospital.db";
        DatabaseManager database(dbPath);
        Hospital hospital;
        bool rejected = false;
        try { database.initializeAndLoad(hospital, data, caseRoot + "/missing-users.txt"); }
        catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find(missing) != std::string::npos;
        }
        assert(rejected);
        sqlite3* raw = nullptr;
        assert(sqlite3_open(dbPath.c_str(), &raw) == SQLITE_OK);
        assert(scalarCount(raw, "SELECT COUNT(*) FROM Metadata WHERE key='text_migration_v1'") == 0);
        sqlite3_close(raw);

        writeText(data + "/" + missing, "");
        const DatabaseMigrationSummary retried = database.initializeAndLoad(
            hospital, data, caseRoot + "/missing-users.txt");
        assert(retried.migrated);
        const DatabaseMigrationSummary repeated = database.initializeAndLoad(
            hospital, data, caseRoot + "/missing-users.txt");
        assert(!repeated.migrated);
    }

    {
        const std::string caseRoot = root + "/unreadable";
        const std::string data = caseRoot + "/data";
        createEmptyLegacyData(data);
        std::filesystem::remove(data + "/patients.txt");
        std::filesystem::create_directory(data + "/patients.txt");
        assertFailedMigrationLeavesNoMarker(caseRoot, data, caseRoot + "/missing-users.txt");
    }

    const struct MalformedCase { const char* name; const char* file; const char* record; } malformed[] = {
        {"bad_patient", "patients.txt", "P invalid record\n"},
        {"bad_doctor", "doctors.txt", "D invalid record\n"},
        {"bad_treatment", "treatments.txt", "T invalid record\n"},
        {"duplicate_patient", "patients.txt",
         "P 1 REGULAR 20 1 0 0 WAITING \"One\" \"Check\"\nP 1 REGULAR 20 1 0 0 WAITING \"Two\" \"Check\"\n"},
        {"duplicate_doctor", "doctors.txt",
         "D 1 1 -1 \"One\" \"General\"\nD 1 1 -1 \"Two\" \"General\"\n"},
        {"duplicate_treatment", "treatments.txt",
         "T 1 1 1 \"A\" \"B\"\nT 1 1 1 \"C\" \"D\"\n"}
    };
    for (const MalformedCase& item : malformed) {
        const std::string caseRoot = root + "/" + item.name;
        const std::string data = caseRoot + "/data";
        createEmptyLegacyData(data);
        if (std::string(item.name) == "duplicate_treatment") {
            writeText(data + "/patients.txt",
                "P 1 REGULAR 20 1 0 0 COMPLETED \"Completed\" \"Check\"\n");
            writeText(data + "/doctors.txt", "D 1 1 -1 \"Doctor\" \"General\"\n");
        }
        writeText(data + "/" + item.file, item.record);
        assertFailedMigrationLeavesNoMarker(caseRoot, data, caseRoot + "/missing-users.txt");
    }

    {
        const std::string caseRoot = root + "/malformed_users";
        const std::string data = caseRoot + "/data";
        createEmptyLegacyData(data);
        const std::string userFixture = caseRoot + "/users.txt";
        writeText(userFixture, "malformed synthetic account record\n");
        assertFailedMigrationLeavesNoMarker(caseRoot, data, userFixture);
    }

    {
        const std::string caseRoot = root + "/retry_after_parse_failure";
        const std::string data = caseRoot + "/data";
        createEmptyLegacyData(data);
        writeText(data + "/patients.txt", "P invalid record\n");
        const std::string dbPath = caseRoot + "/hospital.db";
        DatabaseManager database(dbPath);
        Hospital hospital;
        bool rejected = false;
        try { database.initializeAndLoad(hospital, data, caseRoot + "/missing-users.txt"); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
        writeText(data + "/patients.txt", "");
        assert(database.initializeAndLoad(hospital, data, caseRoot + "/missing-users.txt").migrated);
    }
}

void testSchemaValidation(const std::string& root) {
    const std::string validPath = root + "/valid_existing/hospital.db";
    {
        DatabaseManager created(validPath);
        Hospital empty;
        created.initializeEmptyDatabase(empty, root + "/fresh_data", root + "/fresh_users.txt");
    }
    {
        DatabaseManager reopened(validPath);
        Hospital loaded;
        reopened.loadHospital(loaded);
        assert(loaded.getStatistics().totalPatients == 0);
    }

    {
        const std::string data = root + "/protected_data";
        createEmptyLegacyData(data);
        const std::string users = root + "/protected_users.txt";
        writeText(users, "synthetic account fixture; contents must remain private\n");
        DatabaseManager protectedDatabase(root + "/protected_init/hospital.db");
        Hospital empty;
        bool refused = false;
        try { protectedDatabase.initializeEmptyDatabase(empty, data, users); }
        catch (const std::runtime_error&) { refused = true; }
        assert(refused);
    }

    {
        const std::string populatedPath = root + "/do_not_replace/hospital.db";
        DatabaseManager populated(populatedPath);
        Hospital existing;
        populated.initializeEmptyDatabase(existing, root + "/no_legacy_data", root + "/no_legacy_users.txt");
        existing.registerRegularPatient(901, "Preserved existing", 30, "Fixture", 2);
        populated.saveHospital(existing);
        bool refused = false;
        try { populated.initializeEmptyDatabase(existing, root + "/no_legacy_data", root + "/no_legacy_users.txt"); }
        catch (const std::runtime_error&) { refused = true; }
        assert(refused);
        Hospital stillThere;
        populated.loadHospital(stillThere);
        assert(stillThere.findPatient(901) != nullptr);
    }

    const struct SchemaCase { const char* name; const char* setup; } cases[] = {
        {"unknown_version", "PRAGMA user_version=88;"},
        {"version_zero_incompatible", "CREATE TABLE Patients(id INTEGER PRIMARY KEY);"},
        {"version_one_missing_columns", "CREATE TABLE Patients(id INTEGER PRIMARY KEY); PRAGMA user_version=1;"}
    };
    for (const SchemaCase& item : cases) {
        const std::string path = root + "/schema_" + item.name + ".db";
        sqlite3* raw = nullptr;
        assert(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
        executeRaw(raw, item.setup);
        sqlite3_close(raw);
        bool rejected = false;
        try { DatabaseManager invalid(path); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }

    const std::string missingCheck = root + "/schema_missing_check.db";
    std::filesystem::copy_file(validPath, missingCheck,
        std::filesystem::copy_options::overwrite_existing);
    sqlite3* noCheck = nullptr;
    assert(sqlite3_open(missingCheck.c_str(), &noCheck) == SQLITE_OK);
    executeRaw(noCheck,
        "PRAGMA writable_schema=ON;"
        "UPDATE sqlite_master SET sql=replace(sql,'CHECK(id > 0),','') "
        "WHERE type='table' AND name='Patients';"
        "PRAGMA schema_version=7;"
        "PRAGMA writable_schema=OFF;");
    sqlite3_close(noCheck);
    bool missingCheckRejected = false;
    try { DatabaseManager invalid(missingCheck); }
    catch (const std::runtime_error&) { missingCheckRejected = true; }
    assert(missingCheckRejected);

    const std::string missingIndex = root + "/schema_missing_index.db";
    std::filesystem::copy_file(validPath, missingIndex,
        std::filesystem::copy_options::overwrite_existing);
    sqlite3* noIndex = nullptr;
    assert(sqlite3_open(missingIndex.c_str(), &noIndex) == SQLITE_OK);
    executeRaw(noIndex, "DROP INDEX idx_patients_status");
    sqlite3_close(noIndex);
    bool missingIndexRejected = false;
    try { DatabaseManager invalid(missingIndex); }
    catch (const std::runtime_error&) { missingIndexRejected = true; }
    assert(missingIndexRejected);

    const std::string missingAssignmentIndex = root + "/schema_missing_assignment_index.db";
    std::filesystem::copy_file(validPath, missingAssignmentIndex,
        std::filesystem::copy_options::overwrite_existing);
    sqlite3* noAssignmentIndex = nullptr;
    assert(sqlite3_open(missingAssignmentIndex.c_str(), &noAssignmentIndex) == SQLITE_OK);
    executeRaw(noAssignmentIndex, "DROP INDEX idx_assignments_patient");
    sqlite3_close(noAssignmentIndex);
    bool missingAssignmentIndexRejected = false;
    try { DatabaseManager invalid(missingAssignmentIndex); }
    catch (const std::runtime_error&) { missingAssignmentIndexRejected = true; }
    assert(missingAssignmentIndexRejected);
}

void testLifecyclePersistence(const std::string& databasePath) {
    DatabaseManager database(databasePath);
    Hospital hospital;
    hospital.registerEmergencyPatient(101, "Completed", 40, "A", 5, true);
    hospital.registerEmergencyPatient(102, "Called", 35, "B", 4, true);
    hospital.registerEmergencyPatient(103, "Assigned", 30, "C", 3, true);
    hospital.registerEmergencyPatient(104, "In treatment", 25, "D", 2, true);
    hospital.registerRegularPatient(105, "Waiting", 20, "E", 1);
    hospital.addDoctor(Doctor(201, "Doctor one", "General"));
    hospital.addDoctor(Doctor(202, "Doctor two", "General"));
    hospital.addDoctor(Doctor(203, "Doctor three", "General"));
    assert(hospital.callNextPatient()->getId() == 101);
    hospital.assignDoctor(101, 203);
    hospital.startTreatment(101);
    hospital.completeTreatment(101, 203, "Diagnosis", "Completed care");
    assert(hospital.findDoctor(203)->isAvailable());
    assert(hospital.callNextPatient()->getId() == 102);
    assert(hospital.callNextPatient()->getId() == 103);
    hospital.assignDoctor(103, 201);
    assert(!hospital.findDoctor(201)->isAvailable());
    assert(hospital.callNextPatient()->getId() == 104);
    hospital.assignDoctor(104, 202);
    hospital.startTreatment(104);
    bool invalidAssignmentRejected = false;
    try { hospital.assignDoctor(105, 203); }
    catch (const std::logic_error&) { invalidAssignmentRejected = true; }
    assert(invalidAssignmentRejected);

    database.saveHospital(hospital);
    std::vector<AssignmentRecord> assignmentRows = database.getAssignments();
    assert(assignmentRows.size() == 3);
    std::size_t completedAssignments = 0;
    std::size_t activeAssignments = 0;
    for (const AssignmentRecord& assignment : assignmentRows) {
        assert(assignment.assignedAtEpochSeconds > 0);
        if (assignment.status == "COMPLETED") {
            ++completedAssignments;
            assert(assignment.completedAtEpochSeconds >= assignment.assignedAtEpochSeconds);
            assert(assignment.patientId == 101 && assignment.doctorId == 203);
        } else {
            assert(assignment.status == "ACTIVE");
            ++activeAssignments;
            assert(assignment.patientId == 103 || assignment.patientId == 104);
            assert(assignment.completedAtEpochSeconds == 0);
        }
    }
    assert(completedAssignments == 1 && activeAssignments == 2);
    database.saveHospital(hospital);
    assert(database.getAssignments().size() == 3);
    Hospital loaded;
    database.loadHospital(loaded);
    assert(loaded.findPatient(101)->getStatus() == PatientStatus::COMPLETED);
    assert(loaded.findPatient(102)->getStatus() == PatientStatus::CALLED);
    assert(loaded.findPatient(103)->getStatus() == PatientStatus::ASSIGNED);
    assert(loaded.findPatient(104)->getStatus() == PatientStatus::UNDER_TREATMENT);
    assert(loaded.findPatient(105)->getStatus() == PatientStatus::WAITING);
    assert(loaded.findDoctor(203)->isAvailable());
    assert(!loaded.findDoctor(201)->isAvailable() && loaded.findDoctor(201)->getCurrentPatientId() == 103);
    assert(!loaded.findDoctor(202)->isAvailable() && loaded.findDoctor(202)->getCurrentPatientId() == 104);
    const HospitalStatistics stats = loaded.getStatistics();
    assert(stats.completedPatients == 1 && stats.calledPatients == 1 && stats.assignedPatients == 1);
    assert(stats.patientsUnderTreatment == 1 && stats.waitingPatients == 1);
    assert(stats.availableDoctors == 1 && stats.busyDoctors == 2);

    loaded.completeTreatment(104, 202, "Diagnosis 2", "Completed care 2");
    assert(loaded.findDoctor(202)->isAvailable());
    database.saveHospital(loaded);
    Hospital completedReload;
    database.loadHospital(completedReload);
    assert(completedReload.findPatient(104)->getStatus() == PatientStatus::COMPLETED);
    assert(completedReload.findDoctor(202)->isAvailable());
    assignmentRows = database.getAssignments();
    assert(assignmentRows.size() == 3);
    completedAssignments = 0;
    activeAssignments = 0;
    for (const AssignmentRecord& assignment : assignmentRows) {
        if (assignment.status == "COMPLETED") ++completedAssignments;
        else if (assignment.status == "ACTIVE") ++activeAssignments;
        else assert(false);
    }
    assert(completedAssignments == 2 && activeAssignments == 1);
    DatabaseManager restarted(databasePath);
    Hospital afterRestart;
    restarted.loadHospital(afterRestart);
    const std::vector<AssignmentRecord> restartedAssignments = restarted.getAssignments();
    assert(restartedAssignments.size() == 3);
    std::size_t restartedCompleted = 0;
    std::size_t restartedActive = 0;
    for (const AssignmentRecord& assignment : restartedAssignments) {
        if (assignment.status == "COMPLETED") ++restartedCompleted;
        else if (assignment.status == "ACTIVE") ++restartedActive;
    }
    assert(restartedCompleted == 2 && restartedActive == 1);
    assert(afterRestart.findPatient(104)->getStatus() == PatientStatus::COMPLETED);

    sqlite3* raw = nullptr;
    assert(sqlite3_open(databasePath.c_str(), &raw) == SQLITE_OK);
    executeRaw(raw, "PRAGMA foreign_keys=ON");
    const int invalidAssignment = sqlite3_exec(raw,
        "INSERT INTO Assignments(patient_id,doctor_id,assigned_at_epoch_seconds,status,completed_at_epoch_seconds,timestamp_inferred) "
        "VALUES(9999,9998,1,'ACTIVE',NULL,0)", nullptr, nullptr, nullptr);
    assert(invalidAssignment == SQLITE_CONSTRAINT);
    const int duplicateActiveAssignment = sqlite3_exec(raw,
        "INSERT INTO Assignments(patient_id,doctor_id,assigned_at_epoch_seconds,status,completed_at_epoch_seconds,timestamp_inferred) "
        "VALUES(103,201,1,'ACTIVE',NULL,0)", nullptr, nullptr, nullptr);
    assert(duplicateActiveAssignment == SQLITE_CONSTRAINT);
    sqlite3_close(raw);
}

void testVersionOneUpgradeAndRollback(const std::string& root) {
    const std::string validPath = root + "/valid_v1_upgrade.db";
    {
        DatabaseManager database(validPath);
        Hospital hospital;
        database.initializeEmptyDatabase(hospital, root + "/no_data", root + "/no_users.txt");
        hospital.registerEmergencyPatient(1001, "Legacy completed", 42, "Fixture", 5, true);
        hospital.registerRegularPatient(1002, "Legacy assigned", 32, "Fixture", 2);
        hospital.addDoctor(Doctor(1101, "Legacy doctor one", "General"));
        hospital.addDoctor(Doctor(1102, "Legacy doctor two", "Emergency"));
        assert(hospital.callNextPatient()->getId() == 1001);
        hospital.assignDoctor(1001, 1101);
        hospital.startTreatment(1001);
        hospital.completeTreatment(1001, 1101, "Diagnosis", "Completed procedure");
        assert(hospital.callNextPatient()->getId() == 1002);
        hospital.assignDoctor(1002, 1102);
        database.saveHospital(hospital);
        UserManager users(database);
        users.registerUser("Upgrade Test", "upgrade_test", "Upgrade-test-pass");
    }

    sqlite3* raw = nullptr;
    assert(sqlite3_open(validPath.c_str(), &raw) == SQLITE_OK);
    executeRaw(raw, "DROP INDEX uq_assignments_active_patient;"
                    "DROP INDEX uq_assignments_active_doctor;"
                    "DROP INDEX idx_assignments_patient;"
                    "DROP INDEX idx_assignments_doctor;"
                    "DROP TABLE Assignments; PRAGMA user_version=1;");
    assert(scalarCount(raw, "SELECT COUNT(*) FROM Patients") == 2);
    assert(scalarCount(raw, "SELECT COUNT(*) FROM Doctors") == 2);
    assert(scalarCount(raw, "SELECT COUNT(*) FROM Treatments") == 1);
    assert(scalarCount(raw, "SELECT COUNT(*) FROM Users") == 1);
    sqlite3_close(raw);

    {
        DatabaseManager upgraded(validPath);
        Hospital hospital;
        upgraded.loadHospital(hospital);
        assert(hospital.getStatistics().totalPatients == 2);
        assert(hospital.findPatient(1001)->getStatus() == PatientStatus::COMPLETED);
        assert(hospital.findPatient(1002)->getStatus() == PatientStatus::ASSIGNED);
        assert(hospital.findDoctor(1101)->isAvailable());
        assert(!hospital.findDoctor(1102)->isAvailable());
        const std::vector<AssignmentRecord> assignments = upgraded.getAssignments();
        assert(assignments.size() == 2);
        bool completedFound = false;
        bool activeFound = false;
        for (const AssignmentRecord& assignment : assignments) {
            assert(assignment.timestampInferred);
            if (assignment.patientId == 1001)
                completedFound = assignment.status == "COMPLETED" && assignment.doctorId == 1101;
            else if (assignment.patientId == 1002)
                activeFound = assignment.status == "ACTIVE" && assignment.doctorId == 1102;
        }
        assert(completedFound && activeFound);
        UserManager users(upgraded);
        assert(users.login("UPGRADE_TEST", "Upgrade-test-pass").user.username == "upgrade_test");
    }
    raw = nullptr;
    assert(sqlite3_open(validPath.c_str(), &raw) == SQLITE_OK);
    assert(scalarCount(raw, "PRAGMA user_version") == 2);
    assert(scalarCount(raw, "SELECT COUNT(*) FROM Assignments") == 2);
    sqlite3_close(raw);

    const std::string retryPath = root + "/retry_v1_upgrade.db";
    {
        DatabaseManager database(retryPath);
        Hospital hospital;
        database.initializeEmptyDatabase(hospital, root + "/retry_no_data", root + "/retry_no_users.txt");
        hospital.registerRegularPatient(1201, "Inconsistent legacy", 28, "Fixture", 2);
        database.saveHospital(hospital);
    }
    raw = nullptr;
    assert(sqlite3_open(retryPath.c_str(), &raw) == SQLITE_OK);
    executeRaw(raw, "UPDATE Patients SET status='ASSIGNED',waiting_started_epoch_seconds=NULL WHERE id=1201;"
                    "DROP INDEX uq_assignments_active_patient;"
                    "DROP INDEX uq_assignments_active_doctor;"
                    "DROP INDEX idx_assignments_patient;"
                    "DROP INDEX idx_assignments_doctor;"
                    "DROP TABLE Assignments; PRAGMA user_version=1;");
    sqlite3_close(raw);
    bool migrationRejected = false;
    try { DatabaseManager invalid(retryPath); }
    catch (const std::runtime_error&) { migrationRejected = true; }
    assert(migrationRejected);
    raw = nullptr;
    assert(sqlite3_open(retryPath.c_str(), &raw) == SQLITE_OK);
    assert(scalarCount(raw, "PRAGMA user_version") == 1);
    assert(scalarCount(raw, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='Assignments'") == 0);
    assert(scalarCount(raw, "SELECT COUNT(*) FROM Patients WHERE id=1201") == 1);
    executeRaw(raw, "UPDATE Patients SET status='WAITING',waiting_started_epoch_seconds=CAST(strftime('%s','now') AS INTEGER) WHERE id=1201");
    sqlite3_close(raw);
    {
        DatabaseManager retried(retryPath);
        Hospital hospital;
        retried.loadHospital(hospital);
        assert(hospital.findPatient(1201)->getStatus() == PatientStatus::WAITING);
        assert(retried.getAssignments().empty());
    }
}

void testUpgradeExistingDatabaseCopy(const std::string& path) {
    const CoreSnapshot before = readCoreSnapshot(path);
    if (before.schemaVersion != 1)
        throw std::runtime_error("The isolated existing-database test requires a version-1 copy.");
    {
        DatabaseManager upgraded(path);
        Hospital hospital;
        upgraded.loadHospital(hospital);
        const std::vector<AssignmentRecord> assignments = upgraded.getAssignments();
        std::size_t expectedAssignments = 0;
        for (const Treatment& treatment : hospital.getTreatments())
            if (treatment.isCompleted()) ++expectedAssignments;
        for (const Doctor& doctor : hospital.getDoctors())
            if (!doctor.isAvailable()) ++expectedAssignments;
        assert(assignments.size() == expectedAssignments);
        for (const AssignmentRecord& assignment : assignments) {
            assert(assignment.timestampInferred);
            assert(assignment.assignedAtEpochSeconds > 0);
        }
        assert(hospital.getStatistics().totalPatients == before.patients.size());
    }
    const CoreSnapshot after = readCoreSnapshot(path);
    assert(after.schemaVersion == 2);
    assert(after.patients == before.patients);
    assert(after.doctors == before.doctors);
    assert(after.treatments == before.treatments);
    assert(after.userCount == before.userCount);
}
}

int main(int argc, char* argv[]) {
    assert(argc > 0);
    const std::filesystem::path locatedRoot = ProjectPaths::projectRoot(argv[0]);
    assert(std::filesystem::is_regular_file(locatedRoot / "CMakeLists.txt"));
    assert(std::filesystem::is_regular_file(locatedRoot / "include" / "DatabaseManager.h"));
    if (argc == 3 && std::string(argv[1]) == "--upgrade-copy") {
        testUpgradeExistingDatabaseCopy(argv[2]);
        std::cout << "Isolated existing database assignment upgrade passed.\n";
        return 0;
    }
    if (argc != 1) throw std::invalid_argument("Unexpected database-test arguments.");
    TemporaryDirectory temporary{makeTemporaryRoot()};
    const std::string root = temporary.path;
    const std::string dataDirectory = root + "/legacy-data";
    const std::string usersFile = root + "/users.txt";
    const std::string databaseFile = root + "/hospital.db";

    Hospital source;
    const auto now = std::chrono::system_clock::now();
    source.registerPatient(std::unique_ptr<Patient>(new EmergencyPatient(
        201, "Test emergency", 40, "Test condition", 5, true, 8)));
    source.registerPatient(std::unique_ptr<Patient>(new RegularPatient(
        202, "Test assigned", 31, "Test condition", 4, 0, now - std::chrono::minutes(6))));
    source.registerPatient(std::unique_ptr<Patient>(new RegularPatient(
        203, "Test waiting", 22, "Test condition", 2, 0, now - std::chrono::minutes(4))));
    source.addDoctor(Doctor(301, "Test doctor one", "General"));
    source.addDoctor(Doctor(302, "Test doctor two", "Emergency"));
    Patient* next = source.callNextPatient();
    assert(next != nullptr && next->getId() == 201);
    source.assignDoctor(201, 301);
    source.startTreatment(201);
    source.completeTreatment(201, 301, "Test diagnosis", "Test completed treatment");
    next = source.callNextPatient();
    assert(next != nullptr && next->getId() == 202);
    source.assignDoctor(202, 302);
    source.startTreatment(202);
    FileManager(dataDirectory).saveAll(source);

    {
        UserManager legacyUsers(usersFile);
        legacyUsers.registerUser("Migrated Test Account", "migrated_test", "Migrated-test-pass");
    }

    DatabaseManager database(databaseFile);
    Hospital migrated;
    const DatabaseMigrationSummary summary = database.initializeAndLoad(
        migrated, dataDirectory, usersFile);
    assert(summary.migrated);
    assert(summary.patients == 3 && summary.doctors == 2 && summary.treatments == 1 && summary.users == 1);
    const HospitalStatistics migratedStats = migrated.getStatistics();
    assert(migratedStats.totalPatients == 3 && migratedStats.waitingPatients == 1);
    assert(migratedStats.calledPatients == 0 && migratedStats.assignedPatients == 0);
    assert(migratedStats.patientsUnderTreatment == 1 && migratedStats.completedPatients == 1);
    assert(migratedStats.availableDoctors == 1 && migratedStats.busyDoctors == 1);
    assert(migratedStats.totalTreatments == 1);
    assert(migrated.findPatient(201)->getStatus() == PatientStatus::COMPLETED);
    assert(migrated.findPatient(202)->getStatus() == PatientStatus::UNDER_TREATMENT);
    assert(migrated.findPatient(203)->getStatus() == PatientStatus::WAITING);
    assert(migrated.findPatient(203)->getWaitingTime() >= 4);
    assert(migrated.findDoctor(301)->isAvailable());
    assert(!migrated.findDoctor(302)->isAvailable());
    assert(migrated.findDoctor(302)->getCurrentPatientId() == 202);
    assert(migrated.getTreatments().size() == 1 && migrated.getTreatments()[0].isCompleted());

    UserManager migratedUsers(database);
    assert(migratedUsers.login("MIGRATED_TEST", "Migrated-test-pass").user.fullName ==
           "Migrated Test Account");
    const AuthUser newUser = migratedUsers.registerUser(
        "Database Test Account", "database_test", "Database-test-pass");
    assert(newUser.username == "database_test");
    UserManager reloadedUsers(database);
    assert(reloadedUsers.login("database_test", "Database-test-pass").user.fullName ==
           "Database Test Account");

    Hospital repeated;
    const DatabaseMigrationSummary second = database.initializeAndLoad(repeated, dataDirectory, usersFile);
    assert(!second.migrated && repeated.getStatistics().totalPatients == 3);

    sqlite3* inconsistentRelationship = nullptr;
    assert(sqlite3_open(databaseFile.c_str(), &inconsistentRelationship) == SQLITE_OK);
    executeRaw(inconsistentRelationship,
        "UPDATE Doctors SET available=0,current_patient_id=203 WHERE id=301");
    sqlite3_close(inconsistentRelationship);
    Hospital rejectedLoad;
    bool inconsistentBusyDoctorRejected = false;
    try { database.loadHospital(rejectedLoad); }
    catch (const std::invalid_argument&) { inconsistentBusyDoctorRejected = true; }
    assert(inconsistentBusyDoctorRejected);
    assert(rejectedLoad.getStatistics().totalPatients == 0);
    inconsistentRelationship = nullptr;
    assert(sqlite3_open(databaseFile.c_str(), &inconsistentRelationship) == SQLITE_OK);
    executeRaw(inconsistentRelationship,
        "UPDATE Doctors SET available=1,current_patient_id=NULL WHERE id=301");
    sqlite3_close(inconsistentRelationship);
    Hospital afterRollback;
    database.loadHospital(afterRollback);
    assert(afterRollback.getStatistics().totalPatients == 3);
    assert(afterRollback.findPatient(202)->getStatus() == PatientStatus::UNDER_TREATMENT);

    Hospital duplicate;
    duplicate.registerRegularPatient(501, "First", 20, "Condition", 1);
    bool duplicateRejected = false;
    try { duplicate.registerRegularPatient(501, "Duplicate", 20, "Condition", 1); }
    catch (const std::invalid_argument&) { duplicateRejected = true; }
    assert(duplicateRejected);

    sqlite3* raw = nullptr;
    assert(sqlite3_open(databaseFile.c_str(), &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(scalarCount(raw,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('Patients','Doctors','Treatments','Users','Metadata','Assignments')") == 6);
    assert(scalarCount(raw, "PRAGMA user_version") == 2);
    const int foreignKeyResult = sqlite3_exec(raw,
        "INSERT INTO Doctors(id,name,specialization,available,current_patient_id) VALUES(998,'Bad','General',0,998)",
        nullptr, nullptr, nullptr);
    assert(foreignKeyResult == SQLITE_CONSTRAINT);
    const int assignmentForeignKeyResult = sqlite3_exec(raw,
        "INSERT INTO Assignments(patient_id,doctor_id,assigned_at_epoch_seconds,status,completed_at_epoch_seconds,timestamp_inferred) "
        "VALUES(9998,9999,1,'ACTIVE',NULL,0)", nullptr, nullptr, nullptr);
    assert(assignmentForeignKeyResult == SQLITE_CONSTRAINT);
    sqlite3_close(raw);

    const std::string legacyRoot = root + "/legacy_timestamp";
    const std::string legacyData = legacyRoot + "/data";
    std::filesystem::create_directories(legacyData);
    const std::string legacyPatients = legacyData + "/patients.txt";
    const std::string legacyPatientBytes =
        "# P id type age severity wait critical status \"name\" \"condition\"\n"
        "P 701 REGULAR 28 2 0 0 WAITING \"Legacy Patient\" \"Legacy Condition\"\n";
    writeText(legacyPatients, legacyPatientBytes);
    writeText(legacyData + "/doctors.txt", "# D id available currentPatientId \"name\" \"specialization\"\n");
    writeText(legacyData + "/treatments.txt", "# T patientId doctorId completed \"diagnosis\" \"description\"\n");
    const std::string legacyDbPath = legacyRoot + "/hospital.db";
    {
        DatabaseManager legacyDatabase(legacyDbPath);
        Hospital legacyLoaded;
        const DatabaseMigrationSummary legacySummary = legacyDatabase.initializeAndLoad(
            legacyLoaded, legacyData, legacyRoot + "/missing-users.txt");
        assert(legacySummary.migrated && legacySummary.patients == 1);
        assert(legacyLoaded.findPatient(701) != nullptr);
        assert(legacyLoaded.findPatient(701)->getWaitingTime() == 0);
        assert(readBytes(legacyPatients) == legacyPatientBytes);
        Hospital legacyRestart;
        legacyDatabase.loadHospital(legacyRestart);
        assert(legacyRestart.findPatient(701)->getWaitingTime() == 0);
        assert(readBytes(legacyPatients) == legacyPatientBytes);
    }

    const std::string malformedRoot = root + "/malformed";
    const std::string malformedData = malformedRoot + "/data";
    std::filesystem::create_directories(malformedData);
    writeText(malformedData + "/patients.txt", "P invalid record\n");
    writeText(malformedData + "/doctors.txt", "# D id available currentPatientId \"name\" \"specialization\"\n");
    writeText(malformedData + "/treatments.txt", "# T patientId doctorId completed \"diagnosis\" \"description\"\n");
    {
        DatabaseManager malformedDatabase(malformedRoot + "/hospital.db");
        Hospital malformedHospital;
        bool malformedRejected = false;
        try { malformedDatabase.initializeAndLoad(malformedHospital, malformedData,
                                                    malformedRoot + "/missing-users.txt"); }
        catch (const std::runtime_error&) { malformedRejected = true; }
        assert(malformedRejected && malformedHospital.getStatistics().totalPatients == 0);
        sqlite3* malformedRaw = nullptr;
        assert(sqlite3_open((malformedRoot + "/hospital.db").c_str(), &malformedRaw) == SQLITE_OK);
        assert(scalarCount(malformedRaw, "SELECT COUNT(*) FROM Patients") == 0);
        assert(scalarCount(malformedRaw, "SELECT COUNT(*) FROM Metadata") == 0);
        sqlite3_close(malformedRaw);
    }

    source.completeTreatment(202, 302, "Second test diagnosis", "Second completed treatment");
    database.saveHospital(source);
    Hospital restarted;
    DatabaseManager restartedDatabase(databaseFile);
    const DatabaseMigrationSummary restartSummary = restartedDatabase.initializeAndLoad(
        restarted, dataDirectory, usersFile);
    assert(!restartSummary.migrated);
    assert(restarted.findPatient(202)->getStatus() == PatientStatus::COMPLETED);
    assert(restarted.findDoctor(302)->isAvailable());
    assert(restarted.getTreatments().size() == 2);

    testMigrationSafety(root + "/migration_safety");
    testSchemaValidation(root + "/schema_validation");
    testLifecyclePersistence(root + "/lifecycle_persistence/hospital.db");
    testVersionOneUpgradeAndRollback(root + "/version_upgrade");

    std::cout << "SQLite database tests passed.\n";
    return 0;
}
