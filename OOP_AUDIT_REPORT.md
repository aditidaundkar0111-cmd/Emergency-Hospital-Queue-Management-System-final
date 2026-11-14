# OOP and Code Quality Audit

**Project:** Emergency Hospital Queue Optimizer  
**Audit scope:** C++ domain model, persistence, authentication, HTTP backend, build configuration, tests, and project documentation in this backup. This is a source review; no tests or browser session were run as part of this audit. The original project and runtime data were not accessed. `data/users.txt` was not opened or displayed.

## 1. Project architecture

The application has three layers:

- **Domain and services (`include/`, `src/`):** `Hospital` coordinates patient lifecycle changes and composes patient ownership, queue ordering, doctor management, treatment records, and statistics.
- **Persistence (`DatabaseManager`, with legacy `FileManager`):** SQLite is the runtime store. `DatabaseManager` owns schema setup/version validation, migration, prepared statements, transactions, and model/row conversion. `FileManager` retains the legacy text format and migration/test support.
- **Adapters and presentation:** `WebServer` maps HTTP/JSON requests to domain operations and serves the static `web/` application. `main.cpp` provides the console interface; `web_server_main.cpp` is the web entry point.

This is a sensible academic architecture. The SQLite and HTTP components are relatively large, but the domain model and service responsibilities remain visible rather than being replaced with SQL or route logic throughout the project.

## 2. Actual class hierarchy

```text
Person
└── Patient
    ├── EmergencyPatient
    └── RegularPatient
```

`Doctor`, `Treatment`, and the manager/service classes are not in this inheritance hierarchy. `Hospital` composes those services and owns patient objects.

## 3. Four OOP pillars

### Encapsulation — demonstrated, with some boundary leaks

Most domain state is private: `Person` stores `id_` and `name_`; `Patient` stores age, condition, severity, waiting clock, and status; `Doctor` stores availability/current patient; and `Treatment` stores its patient/doctor relationship and details. Read access is primarily through getters. Patient status changes are private and mediated by `Hospital`, which protects the lifecycle during ordinary application use.

There are deliberate `friend` relationships (`Hospital`, `FileManager`, and `DatabaseManager` with `Patient`; persistence friends on managers). These let persistence restore model state efficiently, but they couple storage code to internal representation. This is tolerable at this project size, though it weakens the abstraction boundary.

A notable exception is `Doctor::assignPatient()` and `Doctor::completeTreatment()`: both are public, and mutable `Doctor*` values are returned by `Hospital::findDoctor()` and `DoctorManager::findDoctorById()`. A caller can therefore change doctor availability without changing patient status. `Hospital::addDoctor()` also accepts a `Doctor` already constructed as busy without checking that its patient exists and is in an assigned state. The normal console/API flow goes through `Hospital`, but the public C++ API does not enforce that invariant for every caller.

### Inheritance — meaningful and correctly destructible

`Patient` is a `Person`; emergency and regular patients are both patients. `EmergencyPatient` adds the critical flag and changes priority behavior. `RegularPatient` reuses patient construction and specializes its display. `Person` has a virtual destructor, and `Patient` overrides it, so deleting derived objects through `std::unique_ptr<Patient>` is safe.

### Polymorphism — real runtime polymorphism

`Person::display()` and `Patient::calculatePriority()` are virtual. `EmergencyPatient` overrides both; `RegularPatient` overrides `display()`. Patients are stored as `std::unique_ptr<Patient>` in `Hospital`, and code calls virtual operations through the base type. The queue comparator and display/statistics paths use those virtual methods. `tests/test_main.cpp` also defines a priority-overriding test patient, which demonstrates extensibility.

### Abstraction — present through interfaces, but no abstract base class

The project hides data behind class APIs and services such as `Hospital`, `PatientQueue`, `DoctorManager`, `StatisticsManager`, `FileManager`, `DatabaseManager`, and `WebServer`. However, there is **no pure virtual function and no abstract class**: `Person` and `Patient` are concrete. The project demonstrates implementation hiding and service abstraction, but should not claim an abstract base class or interface based on the current code.

## 4. Additional C++ concepts

- **Constructors/destructors:** Constructors validate identifiers and required fields. `Patient` has overloads for normal and explicitly timestamped construction; `UserManager` has file-backed and database-backed constructors. `DatabaseManager` has an explicit destructor and deletes copying.
- **Overloading/overriding:** Const and non-const lookup overloads exist for patients/doctors; file loading also has private overloads. Overrides are marked with `override`.
- **Static members/functions:** `PriorityCalculator` exposes `static constexpr` weights and a static calculation function. Legacy user-record parsing is static. There are no meaningful mutable global state objects in the reviewed core.
- **STL:** The implementation uses `std::vector`, `std::priority_queue`, `std::unique_ptr`, `std::map`, `std::array`, algorithms (`find`, `remove_if`, `transform`, `all_of`), streams, `std::quoted`, `std::chrono`, and `std::filesystem`.
- **Ownership and RAII:** `Hospital` owns patients through `std::unique_ptr`. `PatientQueue` holds non-owning raw pointers; the member declaration order and `Hospital` cleanup logic keep those pointers valid during normal ownership. SQLite statements and transactions use local RAII wrappers; `DatabaseManager` closes the connection. The web entry point’s declaration order also keeps referenced `Hospital` and `DatabaseManager` alive longer than `WebServer`.
- **Exceptions:** Domain validation uses standard exceptions and `InvalidPatientDataException`. The file loader constructs a temporary `Hospital` and replaces live state only after successful validation. SQLite writes use transactions and prepared statements.
- **Persistence:** SQLite is the runtime source. Schema validation and versioned migration are centralized in `DatabaseManager`; legacy file formats remain for migration and compatibility.

## 5. Findings and limitations

### Medium — Invalid input is mapped to HTTP 409 instead of HTTP 400

**File/function:** `web_server/WebServer.cpp`, `errorStatus()` (around lines 60–70).  
`std::invalid_argument` derives from `std::logic_error`, but the function checks for `logic_error` before `invalid_argument`. Thus validation failures thrown by `requiredValue()` and domain constructors are caught by the generic logic-error branch and returned as 409; the intended 400 branch is unreachable for these exceptions. Invalid credentials and recognized not-found/duplicate cases are separately mapped first.  
**Impact:** Clients receive a misleading conflict status for malformed fields or invalid values.  
**Minimal improvement:** Check `std::invalid_argument` before `std::logic_error`, while retaining the explicit 401/404/duplicate mappings.

### Medium — Doctor lifecycle state can be mutated outside `Hospital`

**Files/functions:** `include/Doctor.h`; `src/Doctor.cpp`, `assignPatient()` and `completeTreatment()`; `src/Hospital.cpp`, `addDoctor()`.  
The public doctor mutators and mutable pointers allow a caller to mark a doctor busy/available without the corresponding patient transition. `Hospital::addDoctor()` does not validate an initially busy doctor’s patient relationship. Persistence validation will reject inconsistent snapshots later, but the in-memory model can become inconsistent first.  
**Impact:** Counts, assignment behavior, or a later save can fail if client code bypasses the normal lifecycle methods.  
**Minimal improvement:** Make state-changing doctor methods private to `Hospital` (or expose intent-level `Hospital` methods only), and validate any preassigned doctor accepted by `Hospital::addDoctor()`.

### Medium — Web startup exceptions are not handled at the entry point

**File/function:** `web_server_main.cpp`, `main()`.  
Database construction, schema validation, migration, and server setup can throw, but unlike `main.cpp`, this entry point has no top-level `try/catch`.  
**Impact:** An invalid database or migration problem can terminate the process without the console application’s concise startup error handling.  
**Minimal improvement:** Add a top-level `std::exception` handler that prints a sanitized startup error and returns a nonzero exit code.

### Low — Legacy text save is not an atomic multi-file operation

**File/function:** `src/FileManager.cpp`, `saveAll()`.  
It truncates and writes patients, doctors, then treatments sequentially. A later write failure can leave a mixed snapshot. The production entry points use transactional SQLite persistence, so this is limited to the retained legacy API and compatibility use.  
**Minimal improvement:** Keep it clearly documented as legacy, or write each file to a checked temporary and replace it only after successful output. A cross-file transaction is not provided by ordinary filesystems.

### Low — User file loading is more permissive than migration parsing

**File/function:** `src/UserManager.cpp`, file-backed `load()`.  
Malformed/truncated rows are skipped or stop parsing without a diagnostic. The explicit migration parser is stricter and the application runtime uses SQLite, so this primarily affects direct legacy-file mode.  
**Minimal improvement:** Report malformed row numbers without including credential fields, or remove the compatibility path if it is no longer required.

### Low — Registration accepts whitespace-only full names at the service boundary

**File/function:** `src/UserManager.cpp`, `registerUser()`.  
It checks `fullName.empty()` and length, but not whitespace-only input. SQLite’s `Users` constraint rejects a whitespace-only value later; in database-backed API registration that becomes a database exception rather than a clean field-validation response. The frontend trims the name, but direct API callers can bypass that UI check.  
**Minimal improvement:** Apply the same blank-text validation used by `Person` before hashing or persistence.

### Low — Handwritten password KDF and local-session limits

**File/functions:** `src/UserManager.cpp`, PBKDF2/SHA-256 helpers and `login()`.  
The project implements PBKDF2-HMAC-SHA256 itself with 12,000 iterations and uses `std::random_device` to make salts and session tokens. This avoids new dependencies and is reasonable to study, but a custom cryptographic implementation and low work factor are not suitable for production accounts. Sessions are in memory and have no expiry; they end on server restart, as documented.  
**Minimal improvement:** Keep the local-academic-only limitation explicit. For any production use, replace the custom KDF/randomness with a maintained platform/library primitive and add session expiry; do not build a hospital deployment on this prototype.

### Low — Treatment history is intentionally limited to one record per patient

**Files/functions:** `src/TreatmentManager.cpp`, `addTreatment()` / `findTreatment()`; SQLite `Treatments` uses patient ID as its key; `README.md`.  
The manager simply appends and searches by patient ID, while the schema and documentation model one treatment per patient. The current lifecycle supports one completion per patient, so this is internally consistent, but it is not a general treatment-history model.  
**Minimal improvement:** No change is required for the current academic scope; retain this limitation in documentation.

### Low — Tests use `assert`

**Files:** `tests/test_main.cpp`, `tests/database_test.cpp`, and `tests/web_api_test.cpp`.  
Assertions can be compiled out when `NDEBUG` is defined (for example, some Release configurations).  
**Minimal improvement:** Ensure test targets undefine `NDEBUG`, or use explicit test checks that remain active in Release builds.

## 6. Minimal improvement priorities

1. Fix `errorStatus()` ordering so invalid input consistently returns 400.
2. Protect doctor state transitions behind `Hospital` and validate busy-doctor insertion.
3. Add top-level startup exception handling to the web entry point.
4. Tighten whitespace validation and preserve the legacy file-save limitation in documentation.
5. Keep the cryptography and one-treatment-per-patient constraints explicitly scoped to an academic local prototype.

These are focused fixes; a redesign or broad refactor is not warranted by this audit.

## 7. Final assessment

The project demonstrates the four OOP pillars accurately: encapsulated state, meaningful inheritance, runtime polymorphism through base-class ownership, and service-level abstraction. It also uses RAII and STL effectively. It does not have abstract classes or pure virtual interfaces, and should describe abstraction accordingly.

**Academic readiness: Good, with minor code-quality fixes recommended.** The domain model and persistence layer are substantial and coherent for an OOP submission. Fix the HTTP exception mapping and doctor-state encapsulation if time permits, and be explicit that authentication and triage are educational/local only. This is not production clinical software.

## Files inspected

- Build/docs: `CMakeLists.txt`, `README.md`, `.gitignore`
- Entry points/API: `main.cpp`, `web_server_main.cpp`, `web_server/WebServer.cpp`
- Headers: `include/DatabaseManager.h`, `include/Doctor.h`, `include/DoctorManager.h`, `include/EmergencyPatient.h`, `include/Exceptions.h`, `include/FileManager.h`, `include/Hospital.h`, `include/Patient.h`, `include/PatientQueue.h`, `include/PatientStatus.h`, `include/Person.h`, `include/PriorityCalculator.h`, `include/ProjectPaths.h`, `include/RegularPatient.h`, `include/StatisticsManager.h`, `include/Treatment.h`, `include/TreatmentManager.h`, `include/UserManager.h`, `include/WebServer.h`
- Implementations: `src/DatabaseManager.cpp`, `src/Doctor.cpp`, `src/DoctorManager.cpp`, `src/EmergencyPatient.cpp`, `src/FileManager.cpp`, `src/Hospital.cpp`, `src/Patient.cpp`, `src/PatientQueue.cpp`, `src/Person.cpp`, `src/PriorityCalculator.cpp`, `src/RegularPatient.cpp`, `src/StatisticsManager.cpp`, `src/Treatment.cpp`, `src/TreatmentManager.cpp`, `src/UserManager.cpp`
- Tests: `tests/test_main.cpp`, `tests/database_test.cpp`, `tests/web_api_test.cpp` (source/test structure reviewed; not executed during this audit)

`data/users.txt`, `hospital.db`, its sidecars, and the database backup were not opened for content review or modified.
