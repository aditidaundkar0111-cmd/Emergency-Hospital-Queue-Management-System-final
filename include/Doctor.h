#pragma once

#include <string>

class DatabaseManager;
class FileManager;
class Hospital;

class Doctor {
public:
    Doctor(int doctorId, const std::string& name, const std::string& specialization);

    int getDoctorId() const;
    const std::string& getName() const;
    const std::string& getSpecialization() const;
    int getCurrentPatientId() const;
    bool isAvailable() const;
    void display() const;

private:
    friend class DatabaseManager;
    friend class FileManager;
    friend class Hospital;

    Doctor(int doctorId, const std::string& name, const std::string& specialization,
           bool available, int currentPatientId);
    void assignPatient(int patientId);
    void completeTreatment();

    int doctorId_;
    std::string name_;
    std::string specialization_;
    bool available_;
    int currentPatientId_;
};
