# ESP32 Cat Feeding System
## Overview

This project implements a smart cat feeder using an ESP32 microcontroller.
It combines motion detection, a servo-controlled food gate, a buzzer alert,
and a web-based user interface.

The feeder operates automatically when a cat is detected and can also be
controlled manually through a browser.


## Hardware Used

| Component | Description |
|---------|-------------|
| ESP32 | ESP-WROOM-32 development board |
| PIR Sensor | AM312 Mini PIR motion sensor |
| Servo Motor | SG90 / SG60 micro servo |
| Buzzer | Active buzzer (ON/OFF type) |
| Power | External 5V supply for servo (common ground with ESP32) |

## Pin Assignment

- ESP32 GPIO12 -> Servo signal
- ESP32 GPIO27 -> PIR sensor output
- ESP32 GPIO14 -> Buzzer
- ESP32 3V3 -> PIR VCC
- ESP32 GND -> PIR GND, Servo GND, Buzzer GND
- External 5V -> Servo VCC

## System States

### STOPPED
- System disabled
- Servo forced CLOSED
- Buzzer OFF
- PIR input ignored

### AUTO_IDLE
- System enabled
- Waiting for confirmed motion
- Servo CLOSED

### AUTO_OPEN
- Confirmed motion detected
- Servo OPEN (90 degrees)
- Servo remains open while motion persists

### FEEDING
- Triggered by FEED NOW button
- Servo OPEN
- Buzzer active
- After 10 seconds:
  - Close only if motion is no longer detected
  - Otherwise remain open and return to AUTO_OPEN
  
---

## Motion Confirmation Logic

To reduce false triggers and long-distance detection, PIR motion is filtered.

Motion is considered VALID only if:
PIR signal stays HIGH continuously for >= MOTION_CONFIRM_MS (e.g. 250 ms)

This filter improves reliability and safety.


## Web Interface Controls

- The ESP32 hosts a web server accessible from a browser on the same network.

- Graphic Showing the time when the cat eat during the day. Each bar represents one feeding session. Each feeding session is logged when the servo closes.


### Buttons


| Button | Function |
|------|---------|
| START | Enables the system |
| STOP | Disables system and closes servo immediately |
| FEED NOW | Opens servo for 10 seconds with buzzer |
| CLOSE NOW | Forces servo closed immediately |



## FEED NOW Logic

- Servo opens immediately
- Buzzer turns ON (slow beep)
- Wait 10 seconds
- If motion is LOW:
  - Close servo
  - Else:
  - Keep servo OPEN

Return to AUTO mode

This prevents closing the feeder while the cat is still present.


## Forced Close Logic

- The CLOSE NOW button overrides all other states.
- Stop buzzer
- Close servo immediately
- Log open duration if applicable
- Return to AUTO_IDLE

## Power Considerations

- Servo must NOT be powered from ESP32 3.3V
- Use an external 5V power supply (minimum 1A recommended)
- All grounds must be connected together

## Known Limitations

- PIR detection range cannot be precisely limited in software

## Recommended physical solutions:

- Narrow tube in front of PIR

- Partial masking of PIR dome

- Directional mounting toward feeding area

## Instalation Diagram

<img width="527" height="380" alt="image" src="https://github.com/user-attachments/assets/d6f6474e-92ea-4dca-ba8a-bd02b9d392fc" />

## [Demo Video](https://youtu.be/S4dvm2XrX7w)



