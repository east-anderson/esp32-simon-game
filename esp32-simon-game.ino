// Simon Memory Game
// Authors: Easton Anderson
// Description: ESP32-S3 FreeRTOS-based Simon game with LCD, RTC, RFID player
//              profiles, persistent high scores, and multi-core input/game logic.
// Notes: All logic written by author. Comments added to explain design choices
//        (FreeRTOS usage, RFID profiles, and high score persistence).

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <RTClib.h>
#include <SPI.h>
#include <MFRC522.h>

// -----------------------------------------------------------------------------
// Macros / Constants
// -----------------------------------------------------------------------------

#define NUM_BTNS 4          // Number of Simon buttons
const int MAX_LEVEL = 32;   // Maximum pattern length supported

// LED / Button / Buzzer pins
const int ledPins[NUM_BTNS]    = {2, 4, 6, 8};
const int buttonPins[NUM_BTNS] = {3, 5, 7, 9};
const int tonesHz[NUM_BTNS]    = {400, 500, 600, 700};
const int buzzerPin            = 10;

// RFID pins
const int RFID_SS_PIN  = 14;
const int RFID_RST_PIN = 21;

// -----------------------------------------------------------------------------
// Global Objects
// -----------------------------------------------------------------------------

// LCD (I2C 0x27, 16x2)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Real-time clock
RTC_DS3231 rtc;

// RFID reader
MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);

// Preferences for storing per-player highscores
Preferences prefs;

// Current player state
String   currentPlayer = "DEFAULT"; // RFID UID or "DEFAULT" for local profile
int      highScore     = 0;         // Best level reached for this profile
uint32_t highScoreTime = 0;         // Unix timestamp when high score was set

// Game pattern
int pattern[MAX_LEVEL];             // Random sequence of button indices

// Queue to send button presses from TaskButtons -> TaskGame
QueueHandle_t buttonQueue;

// -----------------------------------------------------------------------------
// Function Prototypes
// -----------------------------------------------------------------------------

// Utility
String uidToString(const MFRC522::Uid &uid);

// LED animations
void ledAnim_Player1();
void ledAnim_Player2();
void ledAnim_Default();

// Audio helpers
void playIntroSong();
void playMenuStinger();
void playScanSound_Player1();
void playScanSound_Player2();
void playScanSound_Default();
void playGameOverJingle();
void playGameOverFlash();
void playHighScoreAnimation();
void playStep(int btn);

// High score / time display
void showHighScoreTime();

// Player profile helpers
void loadPlayerProfile(String id);
void savePlayerProfile(String id, int score, uint32_t time);

// FreeRTOS tasks
void TaskButtons(void *pv);
void TaskGame(void *pv);

// -----------------------------------------------------------------------------
// Function Implementations
// -----------------------------------------------------------------------------

/**
 * @brief Convert an MFRC522 UID struct into an uppercase hex string.
 *
 * Example output: "F9771F03"
 *
 * @param uid Reference to the MFRC522::Uid struct read from the tag.
 * @return Uppercase hex string representation of the UID.
 */
String uidToString(const MFRC522::Uid &uid) {
  String out;
  for (byte i = 0; i < uid.size; i++) {
    // Ensure 2 hex characters per byte (leading zero if needed)
    if (uid.uidByte[i] < 0x10) out += "0";
    out += String(uid.uidByte[i], HEX);
  }
  out.toUpperCase();
  return out;
}

/**
 * @brief LED animation used when Player 1 scans their card.
 *
 * Lights LEDs in ascending order, then all on, producing a simple "ramp up"
 * effect that visually differentiates Player 1.
 */
void ledAnim_Player1() {
  // Sweep across LEDs one by one
  for (int i = 0; i < NUM_BTNS; i++) {
    digitalWrite(ledPins[i], HIGH);
    vTaskDelay(pdMS_TO_TICKS(70));
    digitalWrite(ledPins[i], LOW);
  }

  // Flash all LEDs once at the end
  for (int i = 0; i < NUM_BTNS; i++)
    digitalWrite(ledPins[i], HIGH);
  vTaskDelay(pdMS_TO_TICKS(120));
  for (int i = 0; i < NUM_BTNS; i++)
    digitalWrite(ledPins[i], LOW);
}

/**
 * @brief LED animation used when Player 2 scans their card.
 *
 * Lights LEDs in descending order, then flashes all LEDs twice. This pattern
 * visually distinguishes Player 2 from Player 1.
 */
void ledAnim_Player2() {
  // Sweep in reverse order
  for (int i = NUM_BTNS - 1; i >= 0; i--) {
    digitalWrite(ledPins[i], HIGH);
    vTaskDelay(pdMS_TO_TICKS(80));
    digitalWrite(ledPins[i], LOW);
  }

  // Flash all LEDs twice
  for (int k = 0; k < 2; k++) {
    for (int i = 0; i < NUM_BTNS; i++)
      digitalWrite(ledPins[i], HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));
    for (int i = 0; i < NUM_BTNS; i++)
      digitalWrite(ledPins[i], LOW);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/**
 * @brief Default LED animation for unknown / local profiles.
 *
 * Alternates inside and outside LEDs, then flashes all LEDs once.
 */
void ledAnim_Default() {
  // Middle pair
  digitalWrite(ledPins[1], HIGH);
  digitalWrite(ledPins[2], HIGH);
  vTaskDelay(pdMS_TO_TICKS(120));
  digitalWrite(ledPins[1], LOW);
  digitalWrite(ledPins[2], LOW);

  // Outer pair
  digitalWrite(ledPins[0], HIGH);
  digitalWrite(ledPins[3], HIGH);
  vTaskDelay(pdMS_TO_TICKS(120));
  digitalWrite(ledPins[0], LOW);
  digitalWrite(ledPins[3], LOW);

  // All LEDs on briefly
  for (int i = 0; i < NUM_BTNS; i++)
    digitalWrite(ledPins[i], HIGH);
  vTaskDelay(pdMS_TO_TICKS(150));
  for (int i = 0; i < NUM_BTNS; i++)
    digitalWrite(ledPins[i], LOW);
}

/**
 * @brief Play a short intro melody at power up.
 *
 * Uses four ascending notes to indicate the system is ready.
 */
void playIntroSong() {
  int notes[] = {392, 523, 659, 784};
  for (int i = 0; i < 4; i++) {
    tone(buzzerPin, notes[i]);
    vTaskDelay(pdMS_TO_TICKS(200));
    noTone(buzzerPin);
    vTaskDelay(pdMS_TO_TICKS(60));
  }
}

/**
 * @brief Play a short "menu stinger" sound when entering the main menu.
 */
void playMenuStinger() {
  tone(buzzerPin, 784);
  vTaskDelay(pdMS_TO_TICKS(120));
  noTone(buzzerPin);
  vTaskDelay(pdMS_TO_TICKS(20));

  tone(buzzerPin, 988);
  vTaskDelay(pdMS_TO_TICKS(120));
  noTone(buzzerPin);
}

/**
 * @brief Play scan sound and LED animation for Player 1 badge.
 *
 * @note Calls ledAnim_Player1() to combine visuals and audio.
 */
void playScanSound_Player1() {
  ledAnim_Player1();
  int notes[] = {523, 659, 784, 1046};
  int times[] = {140, 140, 140, 180};
  for (int i = 0; i < 4; i++) {
    tone(buzzerPin, notes[i]);
    vTaskDelay(pdMS_TO_TICKS(times[i]));
    noTone(buzzerPin);
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

/**
 * @brief Play scan sound and LED animation for Player 2 badge.
 *
 * Uses descending notes and reverse LED sweep.
 */
void playScanSound_Player2() {
  ledAnim_Player2();
  int notes[] = {880, 740, 659, 554};
  int times[] = {160, 160, 160, 200};
  for (int i = 0; i < 4; i++) {
    tone(buzzerPin, notes[i]);
    vTaskDelay(pdMS_TO_TICKS(times[i]));
    noTone(buzzerPin);
    vTaskDelay(pdMS_TO_TICKS(45));
  }
}

/**
 * @brief Play scan sound and LED animation for default/local profile.
 */
void playScanSound_Default() {
  ledAnim_Default();
  int notes[] = {660, 784, 660};
  int times[] = {150, 150, 180};
  for (int i = 0; i < 3; i++) {
    tone(buzzerPin, notes[i]);
    vTaskDelay(pdMS_TO_TICKS(times[i]));
    noTone(buzzerPin);
    vTaskDelay(pdMS_TO_TICKS(35));
  }
}

/**
 * @brief Play "game over" descending jingle.
 */
void playGameOverJingle() {
  int notes[] = {784, 659, 523, 392};
  for (int i = 0; i < 4; i++) {
    tone(buzzerPin, notes[i]);
    vTaskDelay(pdMS_TO_TICKS(200));
    noTone(buzzerPin);
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

/**
 * @brief Flash all LEDs with a low buzzer tone for game over effect.
 *
 * Flashes three times while holding a low frequency tone.
 */
void playGameOverFlash() {
  for (int k = 0; k < 3; k++) {
    tone(buzzerPin, 200);
    for (int i = 0; i < NUM_BTNS; i++) digitalWrite(ledPins[i], HIGH);
    vTaskDelay(pdMS_TO_TICKS(200));
    noTone(buzzerPin);
    for (int i = 0; i < NUM_BTNS; i++) digitalWrite(ledPins[i], LOW);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

/**
 * @brief Animation and sound for setting a new high score.
 */
void playHighScoreAnimation() {
  int notes[] = {523, 659, 784, 988};

  // Ascending short melody
  for (int i = 0; i < 4; i++) {
    tone(buzzerPin, notes[i]);
    vTaskDelay(pdMS_TO_TICKS(180));
    noTone(buzzerPin);
    vTaskDelay(pdMS_TO_TICKS(40));
  }

  // "Wave" across LEDs with a bright tone
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < NUM_BTNS; i++) {
      digitalWrite(ledPins[i], HIGH);
      tone(buzzerPin, 880);
      vTaskDelay(pdMS_TO_TICKS(80));
      digitalWrite(ledPins[i], LOW);
      noTone(buzzerPin);
    }
  }

  // Final all-on flash
  for (int i = 0; i < NUM_BTNS; i++) digitalWrite(ledPins[i], HIGH);
  tone(buzzerPin, 1046);
  vTaskDelay(pdMS_TO_TICKS(250));
  noTone(buzzerPin);
  for (int i = 0; i < NUM_BTNS; i++) digitalWrite(ledPins[i], LOW);
}

/**
 * @brief Play a single step in the Simon pattern.
 *
 * Lights the corresponding LED and plays its tone, then turns it off.
 *
 * @param btn Index of the button (0..NUM_BTNS-1).
 */
void playStep(int btn) {
  digitalWrite(ledPins[btn], HIGH);
  tone(buzzerPin, tonesHz[btn]);
  vTaskDelay(pdMS_TO_TICKS(300));
  digitalWrite(ledPins[btn], LOW);
  noTone(buzzerPin);
  vTaskDelay(pdMS_TO_TICKS(150));
}

/**
 * @brief Display the timestamp of the current high score on the LCD.
 *
 * Format: "MM/DD HH:MM" (local RTC time).
 * If highScoreTime is 0, prints "No Time".
 */
void showHighScoreTime() {
  if (highScoreTime == 0) {
    lcd.print("No Time");
    return;
  }

  // Convert stored Unix time back to DateTime
  DateTime t(highScoreTime);
  char buf[17];
  snprintf(buf, 16, "%02d/%02d %02d:%02d", t.month(), t.day(), t.hour(), t.minute());
  lcd.print(buf);
}

/**
 * @brief Load high score and timestamp for a given player ID from NVS.
 *
 * @param id Player ID string (RFID UID or "DEFAULT").
 */
void loadPlayerProfile(String id) {
  currentPlayer  = id;
  highScore      = prefs.getInt(("high_" + id).c_str(), 0);
  highScoreTime  = prefs.getULong(("time_" + id).c_str(), 0);
}

/**
 * @brief Save high score and timestamp for a given player ID into NVS.
 *
 * @param id    Player ID string (RFID UID or "DEFAULT").
 * @param score High score to persist.
 * @param time  Unix timestamp when high score was achieved.
 */
void savePlayerProfile(String id, int score, uint32_t time) {
  prefs.putInt(("high_" + id).c_str(), score);
  prefs.putULong(("time_" + id).c_str(), time);
}

/**
 * @brief Task running on Core 0 that debounces physical buttons and pushes
 *        button indices into a FreeRTOS queue.
 *
 * This isolates noisy button input from game logic and provides a clean
 * stream of "button pressed" events to TaskGame.
 *
 * @param pv Unused task parameter.
 */
void TaskButtons(void *pv) {
  // Last stable state for each button (HIGH = not pressed due to INPUT_PULLUP)
  int lastState[NUM_BTNS] = {HIGH, HIGH, HIGH, HIGH};

  while (true) {
    for (int i = 0; i < NUM_BTNS; i++) {
      int r = digitalRead(buttonPins[i]);

      // Detect a change in state (potential edge)
      if (r != lastState[i]) {
        // Simple debounce: wait, then confirm the state again
        vTaskDelay(pdMS_TO_TICKS(10));
        int c = digitalRead(buttonPins[i]);
        if (c == r) {
          // Rising edge: HIGH -> LOW means button press with INPUT_PULLUP
          if (r == LOW && lastState[i] == HIGH) {
            // Send the button index to the queue (non-blocking)
            xQueueSend(buttonQueue, &i, 0);
          }
          // Update stored state after debouncing
          lastState[i] = r;
        }
      }
    }

    // Small delay to avoid busy-waiting and reduce CPU usage
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

/**
 * @brief Main game task running on Core 1.
 *
 * Implements a simple state machine with:
 *  - MAIN_MENU: wait for RFID card or local button press to select profile
 *  - PLAYER_MENU: show current player info, allow play or clear high score
 *  - GAME LOOP: generate random pattern, play sequence, and check user input
 *  - POST-GAME: show game over screen, high score info, and replay options
 *
 * Communication:
 *  - Receives button presses via buttonQueue (from TaskButtons).
 *  - Uses Preferences (NVS) and RTC for high score persistence.
 *
 * @param pv Unused task parameter.
 */
void TaskGame(void *pv) {

  while (true) {

MAIN_MENU:

    playMenuStinger();
    xQueueReset(buttonQueue);  // Clear any stale button events
    bool profileSelected = false;

    // ------------------------- Profile Selection -------------------------
    while (!profileSelected) {

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("   SIMON GAME");
      lcd.setCursor(0, 1);
      lcd.print("Tap card/Press 1");

      // --- RFID scan path (player card) ---
      if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {

        String uid = uidToString(mfrc522.uid);
        loadPlayerProfile(uid);

        // Hard-coded UID mapping for two known players
        if (uid == "F9771F03")      playScanSound_Player1();
        else if (uid == "EA53AE02") playScanSound_Player2();
        else                        playScanSound_Default();

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Player:");
        lcd.print(uid.substring(0, 8)); // Truncate to fit on LCD

        lcd.setCursor(0, 1);
        lcd.print("HS:");
        lcd.print(highScore);
        vTaskDelay(pdMS_TO_TICKS(1500));

        profileSelected = true;
        break;
      }

      // --- Local play path (no RFID) ---
      int btn;
      if (xQueueReceive(buttonQueue, &btn, pdMS_TO_TICKS(60))) {
        if (btn == 0) {
          // Use a shared "DEFAULT" profile if player presses button 1
          loadPlayerProfile("DEFAULT");
          playScanSound_Default();

          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Local Profile");
          lcd.setCursor(0, 1);
          lcd.print("HS:");
          lcd.print(highScore);
          vTaskDelay(pdMS_TO_TICKS(1200));

          profileSelected = true;
        }
      }
    }

PLAYER_MENU:

    bool startGame = false;

    // ----------------------------- Player Menu -----------------------------
    while (!startGame) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Player:");
      lcd.print(currentPlayer.substring(0, 8));

      lcd.setCursor(0, 1);
      lcd.print("1:Play  2:Clear");

      int btn;
      // Block until a button event arrives
      xQueueReceive(buttonQueue, &btn, portMAX_DELAY);

      if (btn == 0) {
        // Start game
        startGame = true;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("   Get Ready!");
        vTaskDelay(pdMS_TO_TICKS(800));
      }
      else if (btn == 1) {
        // Clear high score for current profile
        highScore     = 0;
        highScoreTime = 0;
        savePlayerProfile(currentPlayer, 0, 0);

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("   High Score");
        lcd.setCursor(0, 1);
        lcd.print("    Cleared!");
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    // ----------------------------- Game Start ------------------------------
    // Generate random pattern for all levels upfront
    for (int i = 0; i < MAX_LEVEL; i++)
      pattern[i] = esp_random() % NUM_BTNS;

    int  level    = 1;     // Current pattern length
    bool gameOver = false;

    // -------------------------- Main Game Loop ----------------------------
    while (!gameOver) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Level:");
      lcd.print(level);
      lcd.setCursor(0, 1);
      lcd.print("Score:");
      lcd.print(level - 1);

      // Play back current pattern to user
      for (int i = 0; i < level; i++)
        playStep(pattern[i]);

      // Make sure input queue is empty before reading user input
      xQueueReset(buttonQueue);

      // Read back the player's attempt, one button at a time
      for (int i = 0; i < level; i++) {
        int btn;
        // Block until the player presses a button
        xQueueReceive(buttonQueue, &btn, portMAX_DELAY);

        // Provide immediate feedback (light + tone)
        digitalWrite(ledPins[btn], HIGH);
        tone(buzzerPin, tonesHz[btn]);
        vTaskDelay(pdMS_TO_TICKS(150));
        digitalWrite(ledPins[btn], LOW);
        noTone(buzzerPin);

        // Check against expected pattern
        if (btn != pattern[i]) {
          gameOver = true;
          break;
        }
      }

      // If the player survived this level, increase difficulty
      if (!gameOver) {
        level++;
        if (level > MAX_LEVEL) level = MAX_LEVEL;
        vTaskDelay(pdMS_TO_TICKS(500));
      }
    }

    // Final score is one less than the failed level
    int finalScore = level - 1;
    int oldHigh    = highScore;

    // ------------------------- High Score Handling -------------------------
    if (finalScore > highScore) {
      highScore     = finalScore;
      highScoreTime = rtc.now().unixtime();
      savePlayerProfile(currentPlayer, highScore, highScoreTime);
    }

    // ----------------------------- Game Over UI ----------------------------
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("   GAME OVER!");
    lcd.setCursor(0, 1);
    lcd.print("    Score:");
    lcd.print(finalScore);

    playGameOverFlash();
    playGameOverJingle();

    // If this run set a new high score, celebrate it
    if (finalScore > oldHigh) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("NEW HIGH SCORE!");
      lcd.setCursor(0, 1);
      lcd.print(finalScore);
      vTaskDelay(pdMS_TO_TICKS(700));
      playHighScoreAnimation();
    }

    // Show best score and when it was achieved
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("High Score:");
    lcd.print(highScore);
    lcd.setCursor(0, 1);
    showHighScoreTime();
    vTaskDelay(pdMS_TO_TICKS(1500));

    // --------------------------- Post-Game Menu ----------------------------
    while (true) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("1:Play Again");
      lcd.setCursor(0, 1);
      lcd.print("2:Main Menu");

      int btn;
      xQueueReceive(buttonQueue, &btn, portMAX_DELAY);

      if (btn == 0) {
        // Restart game with same profile
        goto PLAYER_MENU;
      }
      else if (btn == 1) {
        // Go back and allow a different profile to be selected
        goto MAIN_MENU;
      }
    }
  }
}

/**
 * @brief Arduino setup function. Initializes hardware, peripherals, FreeRTOS
 *        tasks, and plays an intro sound.
 */
void setup() {
  Serial.begin(115200);

  // Initialize I2C for LCD (custom SDA/SCL pins)
  Wire.begin(18, 19);
  lcd.init();
  lcd.backlight();

  // Initialize RTC and set time to compile time
  rtc.begin();
  rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  // Initialize RFID over SPI
  SPI.begin();
  mfrc522.PCD_Init();

  // Initialize preferences storage and load default profile
  prefs.begin("simon", false);
  loadPlayerProfile("DEFAULT");

  // Configure LED and button pins
  for (int i = 0; i < NUM_BTNS; i++) {
    pinMode(ledPins[i], OUTPUT);
    pinMode(buttonPins[i], INPUT_PULLUP);  // Active-low buttons
    digitalWrite(ledPins[i], LOW);
  }

  pinMode(buzzerPin, OUTPUT);

  // Create a queue for button events (stores int indices)
  buttonQueue = xQueueCreate(10, sizeof(int));

  // Spawn tasks pinned to different cores:
  //  - Core 0: Input / debouncing
  //  - Core 1: Game logic & UI
  xTaskCreatePinnedToCore(TaskButtons, "Buttons", 2048, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(TaskGame,   "Game",    4096, NULL, 1, NULL, 1);

  // Play startup melody
  playIntroSong();
}

/**
 * @brief Empty loop; logic is fully handled by FreeRTOS tasks.
 */
void loop() {}
