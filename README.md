# ESP32 Simon Memory Game

A Simon-style memory game built on the ESP32-S3 using FreeRTOS tasks, I2C communication, LED sequencing, and an LCD display. The system implements multi-level gameplay, timing control, button input handling, and persistent high score storage. Developed as a final project for an embedded systems course.

---

## System Diagram

```mermaid
flowchart LR
    classDef esp fill:#f4a259,stroke:#333,stroke-width:1px,color:black,rx:12,ry:12;
    classDef led fill:#b5e8a8,stroke:#333,stroke-width:1px,color:black,rx:12,ry:12;
    classDef btn fill:#a8d8ff,stroke:#333,stroke-width:1px,color:black,rx:12,ry:12;
    classDef lcd fill:#d6b3f9,stroke:#333,stroke-width:1px,color:black,rx:12,ry:12;
    classDef power fill:#ff9a9a,stroke:#333,stroke-width:1px,color:black,rx:12,ry:12;

    LED1[LED 1]:::led
    LED2[LED 2]:::led
    LED3[LED 3]:::led
    LED4[LED 4]:::led

    BTN1[Button 1]:::btn
    BTN2[Button 2]:::btn
    BTN3[Button 3]:::btn
    BTN4[Button 4]:::btn

    ESP[ESP32-S3]:::esp
    LCD[LCD Display (I2C)]:::lcd
    Power[Power Supply (USB-C)]:::power

    LED1 --> ESP
    LED2 --> ESP
    LED3 --> ESP
    LED4 --> ESP

    BTN1 --> ESP
    BTN2 --> ESP
    BTN3 --> ESP
    BTN4 --> ESP

    LCD <-->|SDA/SCL| ESP
    Power --> ESP

---

## Features

- ESP32-S3-based game system
- FreeRTOS tasks for LED control, input timing, and game logic
- I2C LCD display for game messages, scores, and prompts
- Level generation and progression with increasing difficulty
- Button input handling with debouncing
- Persistent high scores stored in non-volatile memory
- Player profiles with saved score and timestamp
- Game-over and restart functionality

---

## Technologies Used

- ESP32-S3
- Arduino framework / ESP-IDF Arduino core
- FreeRTOS
- I2C communication
- LCD1602 (I2C)
- Internal RTC module
- NVS (non-volatile storage)

---

## How the Game Works

1. The game generates a random LED sequence for each level.
2. LEDs blink in order; the player must memorize the pattern.
3. The player repeats the pattern using button input.
4. Correct input advances to the next level.
5. Incorrect input triggers a game-over screen and stores the score.
6. High scores persist using NVS storage.

---

## Hardware Setup

- ESP32-S3 development board  
- LCD1602 with I2C backpack  
- LEDs with current-limiting resistors  
- Push buttons  
- Breadboard or wiring harness  

Pin assignments are defined inside the `.ino` file.

---

## License

This project is licensed under an All Rights Reserved license.
Use, copying, modification, or distribution of this code is prohibited without explicit written permission from the author.
See the LICENSE file for full details.
