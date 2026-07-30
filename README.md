# Accident-Alert-Module
ESP32-based embedded crash alert prototype that detects impact events (MPU6050), gets GPS location (NEO-6M), and sends emergency SMS/call via SIM800L; built with Arduino IDE for university demonstration.

**OR Smart Crash Alert System (ESP32) — Brief Project Notes**

This repository contains an embedded systems code for a prototype for automated crash alerting.  
The system uses an ESP32 with:
- MPU6050 (accelerometer/gyroscope) for crash-like impact detection,
- NEO-6M GPS for location acquisition,
- SIM800L GSM/GPRS for emergency SMS/call dispatch,
- LED + buzzer + button for local alert indication and false-alarm cancellation.

What the system does;
- Continuously monitors motion data.
- Detects potential crash using threshold logic.
- Starts a short confirmation window to allow manual cancel.
- If not cancelled, attempts GPS fix and resolves nearest emergency center (police or hospital).
- Sends emergency SMS and can optionally place a call.

Project scope (important)
This is a purely embedded system for demonstration and learning:
- Firmware on ESP32
- Developed/uploaded using Arduino IDE
- No mobile app
- No web dashboard
- No standalone software platform in this phase

Current status
- Core detection and alert flow implemented.
- Hardware integration completed for controlled testing demonstration.
- Suitable for presentation and controlled testing.

Future improvements (optional)
- Better false-positive filtering/calibration,
- Offline emergency center directory fallback,
- Improved power hardening and handling for GSM reliability,
- Addition of a battery powered version fit to be implemented in outside world scenarios
