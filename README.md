# Emergency Hospital Queue Optimizer

An academic C++17 project demonstrating emergency and regular patient queue management through a console application and a local HTTP/JSON web application. Its priority score is educational and is **not a medically validated triage tool**.

## Technologies

- C++17 with MSYS2 UCRT64 GCC/G++
- SQLite C API (SQLite 3.54.1 in the documented development environment)
- cpp-httplib and nlohmann/json (vendored headers in `third_party/`)
- HTML, CSS, and vanilla JavaScript frontend
- CMake for optional project/test generation; direct UCRT64 compiler commands are also supported

## Features and OOP concepts

- Patient registration and priority-based queue ordering
- Lifecycle: `WAITING → CALLED → ASSIGNED → UNDER_TREATMENT → COMPLETED`
- Doctor assignment and availability tracking
- Treatment records and dashboard statistics
- Local account registration/login with salted password hashes and in-memory bearer sessions
- Encapsulation, inheritance (`Person` → `Patient` → emergency/regular patient), runtime polymorphism, and composition through `Hospital`
- RAII ownership of patients with `std::unique_ptr`; the priority queue contains non-owning pointers

## Project structure

```text
.
├── CMakeLists.txt
├── README.md
├── main.cpp                  # Console entry point
├── web_server_main.cpp       # HTTP server entry point
├── include/                  # Domain and service declarations
├── src/                      # Domain services, persistence, and SQLite database layer
├── web_server/WebServer.cpp  # HTTP API and static frontend hosting
├── web/                      # HTML, CSS, and JavaScript frontend
├── third_party/              # Vendored HTTP and JSON headers/licenses
├── tests/                    # C++ regression, SQLite, HTTP API, and smoke tests
└── data/                     # Legacy/demo text files used for one-time migration
```

## SQLite architecture and schema

`DatabaseManager` owns the SQLite connection, schema initialization, transactions, prepared statements, text-file migration, and mapping between database rows and the existing `Hospital` domain model. The console and web application use `hospital.db` as their runtime source of persistence. Successful console and API operations are saved transactionally. The in-memory `Hospital` continues to provide the existing queue, lifecycle, and statistics behavior; a failed save restores the last committed state before reporting failure.

The schema contains:

- `Patients`: existing patient ID, type, name, age, condition, severity, critical flag, status, frozen wait minutes, and optional arrival epoch for waiting patients.
- `Doctors`: existing doctor ID, name, specialization, availability, and current patient assignment.
- `Treatments`: one treatment per patient, matching the existing `TreatmentManager` model, with doctor relationship and completion status.
- `Assignments`: durable patient/doctor assignment history with assignment and completion epoch timestamps, `ACTIVE`/`COMPLETED` status, and a flag identifying timestamps inferred during an upgrade. Partial unique indexes prevent a patient or doctor from having more than one active assignment.
- `Users`: normalized username key, username, full name, salt, and the existing password hash. Passwords remain hashed; API responses do not expose stored authentication data.
- `Metadata`: schema version and the one-time migration marker.

Foreign keys, primary keys, uniqueness and check constraints protect record relationships. Patient and doctor state remains authoritative for current assignments; the new table records durable assignment history. The existing model does not store treatment timestamps or allow multiple treatment records per patient; the database preserves that behavior.

Version-1 databases are upgraded additively to schema version 2 in one transaction. Existing patient, doctor, treatment, user, and migration records are retained. Active assignments are backfilled from busy doctors and their current patients; completed assignments are backfilled from completed treatments. Version 1 did not store assignment timestamps, so backfilled times use the upgrade time and are marked as inferred. A failed upgrade rolls back the table, indexes, backfill, and version change so startup can retry. Version-2 databases are validated before use.

## First startup and migration

The console and web entry points locate the project root from their executable path and store `hospital.db` there, regardless of the current working directory. On normal first startup, migration reads `data/patients.txt`, `data/doctors.txt`, `data/treatments.txt`, and, if present, `data/users.txt` from that root. All three hospital source files must exist, be readable regular files, and parse successfully; missing or unreadable files stop migration with an explicit path error. A malformed source or failed consistency check rolls back all imported rows and leaves the migration marker unset. Existing IDs, statuses, arrival timestamps, doctor availability/assignments, treatment relationships, and user salts/password hashes are preserved. Legacy waiting-patient records without a timestamp get a fallback timestamp once in the database.

The migration validates records and relationships and inserts the datasets plus the completion marker in one transaction. A failure rolls back and does not modify the source text files. Migration counts are reported without printing usernames, password hashes, or other account contents. Repeated startup sees the marker and does not import duplicates. If a database already contains records but has no migration marker, startup refuses to merge text data. Version-1 databases are checked against the existing schema before the additive upgrade. Version-2 databases are checked for the expected table set, columns, nullability, primary/unique keys, required indexes, CHECK constraints, foreign keys, and referential integrity; unknown versions and unversioned databases containing application objects are rejected without schema recreation.

A genuinely new installation with no legacy hospital or account files must be initialized explicitly with `--initialize-empty`. This option refuses to proceed if any legacy data file exists:

```sh
./emergency_hospital.exe --initialize-empty
# or
./emergency_hospital_web.exe --initialize-empty
```

The executable must be inside the project tree so the project root can be identified from its location. Without the option, a missing `data/` directory or missing source files never marks an empty migration complete. The empty initializer checks the root’s hospital and account-file paths and refuses if any exist.

After successful migration, the text files are retained unchanged and are no longer used for runtime persistence. Database contents are authoritative. The legacy `FileManager` API remains for compatibility and tests but is not used by the application entry points.

`data/patients.txt`, `data/doctors.txt`, and `data/treatments.txt` remain eligible for GitHub as fictional demo fixtures. `data/users.txt` is ignored because it contains account hashes. `hospital.db` and its journal/WAL sidecars are also ignored. Never commit runtime accounts, the database, or database backups.

## Requirements and build

Install/use the matching MSYS2 UCRT64 toolchain and SQLite development package. In the **MSYS2 UCRT64** terminal, make sure GCC and SQLite are available:

```sh
g++ --version
sqlite3 --version
```

From the project root, build the console application:

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -Iinclude -Ithird_party src/*.cpp main.cpp -lsqlite3 -o emergency_hospital.exe
```

Build the production web server (keeps the existing synchronous cpp-httplib configuration):

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -D_WIN32_WINNT=0x0A00 -DCPPHTTPLIB_THREAD_POOL_COUNT=-1 -Iinclude -Ithird_party src/*.cpp web_server/WebServer.cpp web_server_main.cpp -lws2_32 -lsqlite3 -o emergency_hospital_web.exe
```

The direct commands above also work from PowerShell when the UCRT64 runtime directory is on `PATH`:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
```

Optional CMake build and tests:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The C++ regression, SQLite migration, and HTTP API tests use isolated temporary data/database paths. Do not run test executables from the repository root if changing their working-directory setup; the console smoke script should also be run from an isolated working copy.

## Run

Start the console application from the project root:

```sh
./emergency_hospital.exe
```

Start the web application from the project root:

```sh
./emergency_hospital_web.exe
```

Then open [http://localhost:8080](http://localhost:8080). Register a local account and sign in; there is no default account. “Remember Me” remembers the username only. Password recovery is administrator guidance; the project does not implement a reset mechanism.

The web API remains local and synchronous and retains its existing routes and JSON response structures. Hospital routes require a bearer session. Sessions end when the server stops.

## Backup and data location

The production applications use the project-root `hospital.db` path found from the executable location. Stop both applications before copying the database file. For a consistent backup while the application is running, use SQLite’s backup command-line utility from the project root, then store the backup outside the repository:

```sh
sqlite3 hospital.db ".backup 'hospital-backup.db'"
```

Do not commit the database or the backup. Original text files are retained after migration and can be kept as demo fixtures or an archival source.

## Limitations and security assumptions

- This is a local academic prototype, not a production multi-user hospital system.
- The server binds to loopback and processes one HTTP request at a time in the documented configuration.
- Authentication uses the existing project password-hashing implementation; sessions are in memory and have no configured expiry. Do not expose the server to a network or use real patient data.
- The priority weights are educational and must not be used for clinical triage.
- Treatment history is limited to one treatment record per patient by the existing domain model.
