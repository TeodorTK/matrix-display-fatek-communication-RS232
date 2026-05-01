/*
 * File: main.ino
 * Description: Main application logic for LED matrix display and RS232 FATEK communication.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the LICENSE file in the project root for details.
 */

#include <Arduino.h>
#include "FATEKRS232.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <cstring>

/*
    LED Matrix display based on FATEK Modbus registers
    Display condition: M320 = 1 and D0 = 0
    3 text lines:
    1. "Welcome!" - with right-to-left scrolling animation
    2. "Insert" - statică și centrată
    3. "minimum X lei/leu" - statică și centrată (X = D10, leu daca D10=1, lei daca D10>1)
*/

// ========================================
// Translated comment in simple English
// ========================================
#define MODBUS_RX_PIN 16
#define MODBUS_TX_PIN 17
#define MODBUS_BAUDRATE 115200 //7E1 - 7 data bits, Even parity, 1 stop bit (RS232 ASCII FATEK)
#define MODBUS_SLAVE_ID 1

// ========================================
// HUB75 LED MATRIX PARAMETERS
// ========================================
#define PANEL_RES_X 64
#define PANEL_RES_Y 32
#define PANEL_CHAIN 1

// Pins for HUB75 LED matrix (ESP32 default pins)
#define R1_PIN 25
#define G1_PIN 26
#define B1_PIN 27
#define R2_PIN 14
#define G2_PIN 12
#define B2_PIN 13

#define A_PIN 23
#define B_PIN 19
#define C_PIN 18
#define D_PIN 5
#define E_PIN -1  // For panels 64x64, set a valid pin

#define LAT_PIN 32
#define OE_PIN 15
#define CLK_PIN 33

MatrixPanel_I2S_DMA *dma_display = nullptr;

// Display control Serial Monitor
bool enableSerialMonitor = false;  // Set true to show in Serial Monitor, false to disable

// Scroll variables
int scrollX = 0;
int scrollX_line1_scenario3 = 0;  // Scroll for line 1 in scenario 3
int scrollX_line2_scenario3 = 0;  // Scroll for line 2 in scenario 3
int scrollX_scenario5 = 0;  // Scroll for line 1 in scenario 5
int scrollX_line3_scenario12 = 0;  // Scroll for line 3 in scenario 1.2
int scrollX_line3_scenario6 = 0;  // Scroll for line 3 in scenario 6 (PLC communication error)
int scrollX_line3_scenario7 = 0;  // Scroll for line 3 in scenario 7 (M120=0 unavailable)
int line1Width = 0;
int line2Width = 0;
int line3Width = 0;
int longestLineWidth = 0;
uint16_t textColor = 0;
bool displayActive = false;

// Variables for refresh sync
unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_UPDATE_INTERVAL = 10; // Refresh at each 10ms (100 FPS) - refresh rate crescut

// Scroll variables (separate from refresh for independent speed control)
unsigned long lastScrollUpdate = 0;
const unsigned long SCROLL_UPDATE_INTERVAL = 40; // Scroll actualizat at each 40ms
const int SCROLL_EXTRA_SPACES = 5; // Number of extra spaces after text before reset
const int SCROLL_EXTRA_PIXELS = SCROLL_EXTRA_SPACES * 6; // 5 spaces * 6px per character = 30px

// Function to calculate text width
int calcTextWidth(const char *str) {
  return strlen(str) * 6;  // font 5px + 1px spatiu
}

// Helper function to build text from an R register range
String buildTextFromRRange(RegisterValues* regValues, int startIndex, int count) {
  String result = "";
  for (int i = 0; i < count; i++) {
    int regIndex = startIndex + i;
    if (regIndex < 180 && regValues->R_valid[regIndex]) {
      uint16_t regValue = regValues->R[regIndex];
      if (regValue != 0) {  // Ignore if register is 0
        // Extract ASCII characters (each register can hold 2 characters)
        uint8_t lowByte = regValue % 256;
        uint8_t highByte = regValue / 256;
        
        // Low byte (first character)
        if (lowByte >= 32 && lowByte <= 126) {
          result += (char)lowByte;
        }
        
        // High byte (second character)
        if (highByte >= 32 && highByte <= 126) {
          result += (char)highByte;
        }
      }
    }
  }
  return result;
}

// Helper function to get matching color for M71-M79 in scenario 3
uint16_t getColorForMIndex(int mIndex) {
  switch(mIndex) {
    case 0: return dma_display->color565(0, 255, 128);    // M71: (0, 255, 128)
    case 1: return dma_display->color565(0, 128, 255);    // M72: (0, 128, 255)
    case 2: return dma_display->color565(255, 0, 255);    // M73: (255, 0, 255)
    case 3: return dma_display->color565(0, 255, 255);    // M74: (0, 255, 255)
    case 4: return dma_display->color565(128, 255, 0);    // M75: (128, 255, 0)
    case 5: return dma_display->color565(255, 0, 128);    // M76: (255, 0, 128)
    case 6: return dma_display->color565(128, 0, 255);    // M77: (128, 0, 255)
    case 7: return dma_display->color565(0, 255, 0);      // M78: (0, 255, 0)
    case 8: return dma_display->color565(255, 255, 0);    // M79: (255, 255, 0)
    default: return dma_display->color565(255, 255, 255); // Default: alb
  }
}

// Helper function to format time values (00 for 0, 01-09 for 1-9, direct for >=10)
void formatTimeValue(uint16_t value, char* buffer) {
  if (value == 0) {
    strcpy(buffer, "00");
  } else if (value < 10) {
    buffer[0] = '0';
    itoa(value, buffer + 1, 10);
    buffer[2] = '\0';
  } else {
    itoa(value, buffer, 10);
  }
}

// Task for Modbus communication on Core 0
void taskModbusCommunication(void *parameter) {
  // Sync Serial Monitor state with Modbus module
  setSerialMonitorEnabled(enableSerialMonitor);
  
  // Initialization FATEK Modbus
  initFATEKModbus();
  
  // Variables for Serial Monitor display
  unsigned long lastSerialDisplay = 0;
  const unsigned long SERIAL_DISPLAY_INTERVAL = 1000; // Show on Serial once per second
  
  // Variables for communication status check and slave search
  bool wasCommError = false;
  
  // Infinite loop for Modbus communication
  while (true) {
    unsigned long currentTime = millis();
    
    // Sync state Serial Monitor (in case it changed)
    if (getSerialMonitorEnabled() != enableSerialMonitor) {
      setSerialMonitorEnabled(enableSerialMonitor);
    }
    
    
    // Translated comment in simple English
    loopFATEKModbus();
    
    // Show registers on Serial Monitor once per second (only if enabled)
    if (enableSerialMonitor && currentTime - lastSerialDisplay >= SERIAL_DISPLAY_INTERVAL) {
      lastSerialDisplay = currentTime;
      displayRegisterValues();
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));  // Yield control to FreeRTOS
  }
}

// Task for LED matrix display on Core 1
void taskMatrixDisplay(void *parameter) {
  // Configure pins for HUB75 LED matrix
  HUB75_I2S_CFG::i2s_pins _pins = {
    R1_PIN, G1_PIN, B1_PIN,    // R1, G1, B1
    R2_PIN, G2_PIN, B2_PIN,    // R2, G2, B2
    A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,  // Address pins (A, B, C, D, E)
    LAT_PIN, OE_PIN, CLK_PIN   // Latch, Output Enable, Clock
  };
  
  // Initialize Matrix Display with higher refresh rate and configured pins
  HUB75_I2S_CFG matrix_config(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN, _pins);
  matrix_config.min_refresh_rate = 120; // Increase refresh rate to 120 Hz for smoother display
  dma_display = new MatrixPanel_I2S_DMA(matrix_config);
  dma_display->begin();
  dma_display->setBrightness8(255);  // 0-255 Brightness
  dma_display->clearScreen();
  dma_display->setTextWrap(false);
  dma_display->setTextSize(1);
  textColor = dma_display->color565  (128, 128, 255);  // alb
  
  // Infinite loop for display
  while (true) {
    unsigned long currentTime = millis();
    
    // Update display at a fixed interval to avoid flickering
    if (currentTime - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
      lastDisplayUpdate = currentTime;
      
      // Get register values
      RegisterValues* regValues = getRegisterValues();
      
      // Check display conditions
      bool shouldDisplayWelcome = false;  // M320 = 1 and D0 = 0
      bool shouldDisplayWelcome11 = false;  // Scenario 1.1: M322=1 and D0=0
      bool shouldDisplayWelcome12 = false;  // Scenario 1.2: M321=1 and D0=0
      bool shouldDisplayIntroduced = false;  // D0 > 0
      bool shouldDisplayFunction = false;  // Scenario 3: One from M71-M79=1, D0>0, M94=1
      bool shouldDisplayPause = false;  // Scenario 4: M97=1, D0>0
      bool shouldDisplayPauseFinished = false;  // Scenario 5: M99=1, D0>0
      bool shouldDisplayUnavailable = false;  // Scenario 7: M120=0 (unavailable)
      bool shouldDisplayCommError = false;  // Scenario 6: PLC communication error
      int activeMIndex = -1;  // Index-ul M active (0=M71, 1=M72, ..., 8=M79)
      
      // Check Scenario 6 (the more prioritar): PLC communication error
      shouldDisplayCommError = !isCommunicationActive();
      
      // Check Scenario 7 (prioritar, dar after 6): M120=0 → unavailable - only if communication works
      if (!shouldDisplayCommError && regValues->M_valid[15]) {  // M120 is at index 15
        shouldDisplayUnavailable = (regValues->M120 == false);  // M120=0
      }
      
      // Check Scenario 5 (prioritar): M99=1 and D0>0 - only if scenarios 6 and 7 not are active
      if (!shouldDisplayCommError && !shouldDisplayUnavailable && regValues->D_valid[0] && regValues->D0 > 0 && regValues->M_valid[14]) {  // M99 is at index 14
        shouldDisplayPauseFinished = (regValues->M99 == true);
      }
      
      // Check Scenario 4 (higher priority than 3, but after 5): M97=1 and D0>0 - only if scenarios 6 and 7 not are active
      if (!shouldDisplayCommError && !shouldDisplayUnavailable && !shouldDisplayPauseFinished && regValues->D_valid[0] && regValues->D0 > 0 && regValues->M_valid[12]) {  // M97 is at index 12
        shouldDisplayPause = (regValues->M97 == true);
      }
      
      // Check Scenario 3 only if scenarios 4, 5, 6 and 7 not are active
      if (!shouldDisplayCommError && !shouldDisplayUnavailable && !shouldDisplayPause && !shouldDisplayPauseFinished && regValues->D_valid[0] && regValues->D0 > 0) {
        // Check M94 = 1
        bool m94Active = false;
        if (regValues->M_valid[9]) {  // M94 is at index 9 in array
          m94Active = (regValues->M94 == true);
        }
        
        // Check if exact One of M71-M79 is active
        if (m94Active && regValues->M_valid[0] && regValues->M_valid[1] && 
            regValues->M_valid[2] && regValues->M_valid[3] && regValues->M_valid[4] &&
            regValues->M_valid[5] && regValues->M_valid[6] && regValues->M_valid[7] && 
            regValues->M_valid[8]) {
          int activeCount = 0;
          if (regValues->M71) { activeCount++; activeMIndex = 0; }
          if (regValues->M72) { activeCount++; activeMIndex = 1; }
          if (regValues->M73) { activeCount++; activeMIndex = 2; }
          if (regValues->M74) { activeCount++; activeMIndex = 3; }
          if (regValues->M75) { activeCount++; activeMIndex = 4; }
          if (regValues->M76) { activeCount++; activeMIndex = 5; }
          if (regValues->M77) { activeCount++; activeMIndex = 6; }
          if (regValues->M78) { activeCount++; activeMIndex = 7; }
          if (regValues->M79) { activeCount++; activeMIndex = 8; }
          
          if (activeCount == 1) {  // Exact one active
            shouldDisplayFunction = true;
          }
        }
      }
      
      if (regValues->D_valid[0]) {
        // Check Scenario 1: M320=1 and D0=0 (same prioritate with 1.1)
        if (!shouldDisplayCommError && !shouldDisplayUnavailable && regValues->M_valid[16] && regValues->D0 == 0 && !shouldDisplayFunction && !shouldDisplayPause && !shouldDisplayPauseFinished) {
          shouldDisplayWelcome = (regValues->M320 == 1);
        }
        
        // Check Scenario 1.1: M322=1 and D0=0 (same prioritate with 1)
        if (!shouldDisplayCommError && !shouldDisplayUnavailable && regValues->M_valid[18] && regValues->D0 == 0 && !shouldDisplayFunction && !shouldDisplayPause && !shouldDisplayPauseFinished) {
          shouldDisplayWelcome11 = (regValues->M322 == true);  // M322 is at index 18
        }
        
        // Check Scenario 1.2: M321=1 and D0=0 (same prioritate with 1 and 1.1)
        if (!shouldDisplayCommError && !shouldDisplayUnavailable && regValues->M_valid[17] && regValues->D0 == 0 && !shouldDisplayFunction && !shouldDisplayPause && !shouldDisplayPauseFinished) {
          shouldDisplayWelcome12 = (regValues->M321 == true);  // M321 is at index 17
        }
        
        // Check Scenario 2 (D0 > 0)
        if (!shouldDisplayCommError && !shouldDisplayUnavailable && regValues->D0 > 0 && !shouldDisplayFunction && !shouldDisplayPause && !shouldDisplayPauseFinished) {
          shouldDisplayIntroduced = true;
        }
      }
      
      // If scenario 6 is active (communication error), disable all other scenarios
      if (shouldDisplayCommError) {
        shouldDisplayWelcome = false;
        shouldDisplayWelcome11 = false;
        shouldDisplayWelcome12 = false;
        shouldDisplayIntroduced = false;
        shouldDisplayFunction = false;
        shouldDisplayPause = false;
        shouldDisplayPauseFinished = false;
        shouldDisplayUnavailable = false;
      } else if (shouldDisplayUnavailable) {
        // If scenario 7 is active (unavailable), disable all other scenarios (dar not 6)
        shouldDisplayWelcome = false;
        shouldDisplayWelcome11 = false;
        shouldDisplayWelcome12 = false;
        shouldDisplayIntroduced = false;
        shouldDisplayFunction = false;
        shouldDisplayPause = false;
        shouldDisplayPauseFinished = false;
      } else {
        // Scenarios 3 and 4 must be inactive if scenario 5 is active
        if (shouldDisplayPauseFinished) {
          shouldDisplayFunction = false;
          shouldDisplayPause = false;
        }
        
        // Scenario 3 must be inactive if scenario 4 is active
        if (shouldDisplayPause) {
          shouldDisplayFunction = false;
        }
      }
      
      if (shouldDisplayWelcome || shouldDisplayWelcome11 || shouldDisplayWelcome12) {
        // Scenario 1, 1.1 or 1.2: M320=1 and D0=0 Or M322=1 and D0=0 Or M321=1 and D0=0 → message welcome
        // Build cele 3 lines of text
        const char *line1 = "Bine ati venit!";
        char line2[50] = "";
        char line3[50] = "";
        
        // Determine lines 2 and 3 based on scenario
        if (shouldDisplayWelcome12) {
          // Scenario 1.2: M321=1 and D0=0
          strcpy(line2, "Boxa ");
          // Add ID-ul slave-ului
          char slaveIDStr[10];
          itoa(getCurrentSlaveID(), slaveIDStr, 10);
          strcat(line2, slaveIDStr);
          
          strcpy(line3, "Introduceti Bani la Bancomat!");
        } else if (shouldDisplayWelcome11) {
          // Scenario 1.1: M322=1 and D0=0
          strcpy(line2, "Introduceti");
          strcpy(line3, "Jetoane");
        } else {
          // Scenario 1: M320=1 and D0=0
          strcpy(line2, "Introduceti");
          strcpy(line3, "minim ");
          
          // Build line 3 based on D10
          // Check if D10 is valid
          if (regValues->D_valid[1]) {
            char d10Str[20];
            itoa(regValues->D10, d10Str, 10);
            strcat(line3, d10Str);
            
            // D10 == 1 -> "leu", altfel -> "lei"
            if (regValues->D10 == 1) {
              strcat(line3, "Leu");
            } else {
              strcat(line3, "Lei");
            }
          } else {
            strcat(line3, "?Lei");
          }
        }
        
        // Check if use smaller text for line 3 (when D10 > 9, only for scenario 1)
        bool useSmallText = false;
        if (shouldDisplayWelcome && regValues->D_valid[1] && regValues->D10 > 9) {
          useSmallText = true;
        }
        
        // Calculate widths textului
        line1Width = calcTextWidth(line1);
        line2Width = calcTextWidth(line2);
        
        // For line 3, calculate width from text size
        // If use text more small, caracterul default is 5px latime + 1px spatiu = 6px
        // For text more small, vom folosi un factor of scalare of 0.8 (5*0.8 + 1 = 5px per character)
        if (useSmallText) {
          line3Width = (int)(strlen(line3) * 5.0f);  // 5px per character for text more small
        } else {
          line3Width = calcTextWidth(line3);  // 6px per character for text normal
        }
        
        // Calculate positions X - for scenario 1.2 use scroll for lines 1 and 3
        int centerX_line2 = (dma_display->width() - line2Width) / 2;
        int posX_line1, posX_line3;
        
        if (shouldDisplayWelcome12) {
          // For scenario 1.2, lines 1 and 3 au scroll synced (ca in scenario 3)
          // If display-ul not era active or if both lines not are synced
          if (!displayActive) {
            displayActive = true;
            scrollX = dma_display->width();
            scrollX_line3_scenario12 = dma_display->width();
          } else {
            // If both need scrolling, sync them to start together
            // Check if one of ele s-a resetat recently or if are desincronizate
            if (scrollX == dma_display->width() || scrollX_line3_scenario12 == dma_display->width()) {
              // If one resets, also reset the other one
              scrollX = dma_display->width();
              scrollX_line3_scenario12 = dma_display->width();
            }
          }
          
          // Positions for scroll
          posX_line1 = scrollX;
          posX_line3 = scrollX_line3_scenario12;
        } else {
          // For scenarios 1 and 1.1, only line 1 are scroll
          if (!displayActive) {
            scrollX = dma_display->width();
            displayActive = true;
          }
          posX_line1 = scrollX;
          posX_line3 = (dma_display->width() - line3Width) / 2;  // Line 3 centered
        }
        
        // Show text (DMA buffer updates while display remains smooth)
        dma_display->fillScreen(0);
        // Scenario 1, 1.1, 1.2: (255, 255, 255) - alb
        dma_display->setTextColor(dma_display->color565(255, 255, 255));
        
        // Line 1 - with scroll from right to left (synced with line 3 for scenario 1.2)
        dma_display->setCursor(posX_line1, 2);
        dma_display->setTextSize(1);  // Text size normal
        dma_display->print(line1);
        
        // Line 2 - static and centered
        dma_display->setCursor(centerX_line2, 12);
        dma_display->setTextSize(1);  // Text size normal
        dma_display->print(line2);
        
        // Line 3 - scroll for scenario 1.2 (synced with line 1), static and centered for the others (with text more small if D10 > 9)
        dma_display->setCursor(posX_line3, 22);
        if (useSmallText) {
          // Use text size 1 and draw characters one by one
          // to simula un text more small prin reducerea spacing-ului
          dma_display->setTextSize(1);
          // Draw each character with tighter spacing for a smaller-text effect
          for (int i = 0; i < strlen(line3); i++) {
            dma_display->print(line3[i]);
            // Move cursor slightly for a compact text effect
            if (i < strlen(line3) - 1) {
              int16_t x, y;
              uint16_t w, h;
              dma_display->getTextBounds(String(line3[i]), 0, 0, &x, &y, &w, &h);
              dma_display->setCursor(dma_display->getCursorX() - 1, dma_display->getCursorY());
            }
          }
        } else {
          dma_display->setTextSize(1);  // Text size normal
          dma_display->print(line3);
        }
        
        // Update scroll-urile if is necesar (similar to scenario 3)
        if (currentTime - lastScrollUpdate >= SCROLL_UPDATE_INTERVAL) {
          lastScrollUpdate = currentTime;
          
          if (shouldDisplayWelcome12) {
            // For scenario 1.2, use same offset for both lines (synced ca in scenario 3)
            scrollX--;
            scrollX_line3_scenario12 = scrollX;  // Sync at same position
            
            // Reset both only when line 3 (the longest one) fully passes across the screen
            // Line 3 a passed complet when: scrollX_line3_scenario12 + line3Width < 0
            if (scrollX_line3_scenario12 + line3Width < 0) {
              // Line 3 finished scrolling, reset both to start again
              scrollX = dma_display->width();
              scrollX_line3_scenario12 = dma_display->width();
            }
          } else {
            // For scenarios 1 and 1.1, only line 1 are scroll
            scrollX--;
            // Reset when all characters have passed across the screen (ultimul character exits on the left side)
            if (scrollX + line1Width < 0) {
              // All characters have passed across the screen, reset to start again
              scrollX = dma_display->width();
            }
          }
        }
      } else if (shouldDisplayIntroduced) {
        // Scenario 2: D0 > 0 → confirmed input message
        const char *line1 = "Ai";
        const char *line2 = "introdus";
        
        // Build line 3 in formatul "x.x lei/leu"
        // x.x = (D0/10).(D0%10)
        char line3[50] = "";
        
        if (regValues->D_valid[0]) {
          uint16_t d0 = regValues->D0;
          uint16_t parteIntreaga = d0 / 10;  // D0/10 (integer part)
          uint16_t parteZecimala = d0 % 10;  // D0%10 (restul)
          
          // Convert integer part in string
          char intStr[20];
          itoa(parteIntreaga, intStr, 10);
          strcat(line3, intStr);
          
          // Add punctul
          strcat(line3, ".");
          
          // Convert decimal part in string
          char decStr[20];
          itoa(parteZecimala, decStr, 10);
          strcat(line3, decStr);
          
          // Add space and "lei" or "leu"
          strcat(line3, " ");
          if (parteIntreaga == 1) {
            strcat(line3, "Leu");
          } else {
            strcat(line3, "Lei");
          }
        } else {
          strcat(line3, "?.? Lei");
        }
        
        // Calculate widths textului
        line1Width = calcTextWidth(line1);
        line2Width = calcTextWidth(line2);
        line3Width = calcTextWidth(line3);
        
        // If display-ul not era active, reset scroll-ul (not use scroll here)
        if (!displayActive) {
          displayActive = true;
        }
        
        // Calculate positions X for centrare (all lines are statice and centered)
        int centerX_line1 = (dma_display->width() - line1Width) / 2;
        int centerX_line2 = (dma_display->width() - line2Width) / 2;
        int centerX_line3 = (dma_display->width() - line3Width) / 2;
        
        // Show textul
        dma_display->fillScreen(0);
        // Scenario 2: (128, 255, 128) - verde deschis
        dma_display->setTextColor(dma_display->color565(128, 255, 128));
        
        // Line 1 - static and centered
        dma_display->setCursor(centerX_line1, 2);
        dma_display->print(line1);
        
        // Line 2 - static and centered
        dma_display->setCursor(centerX_line2, 10);
        dma_display->print(line2);
        
        // Line 3 - static and centered
        dma_display->setCursor(centerX_line3, 21);
        dma_display->print(line3);
      } else if (shouldDisplayFunction) {
        // Scenario 3: M94=1, Only One from M71-M79=1, D0>0 → function display
        // Mapping M71-M79 -> intervale R
        // M71->R10-R19(line1), R110-R119(line2)
        // M72->R20-R29(line1), R120-R129(line2)
        // M73->R30-R39(line1), R130-R139(line2)
        // M74->R40-R49(line1), R140-R149(line2)
        // M75->R50-R59(line1), R150-R159(line2)
        // M76->R60-R69(line1), R160-R169(line2)
        // M77->R70-R79(line1), R170-R179(line2)
        // M78->R80-R89(line1), R180-R189(line2)
        // M79->R90-R99(line1), R190-R199(line2)
        
        // Calculate indexurile corecte torray-ul R[]
        // R[0-89] = R10-R99, R[90-179] = R110-R199
        // M71 (index 0): R10-R19 (R[0-9]), R110-R119 (R[90-99])
        // M72 (index 1): R20-R29 (R[10-19]), R120-R129 (R[100-109])
        // etc.
        int firstRangeStart = activeMIndex * 10;      // R10-R99 (for line 1): R[0-89]
        int secondRangeStart = 90 + activeMIndex * 10;  // R110-R199 (for line 2): R[90-179]
        
        // Build textul for each interval
        String text1 = buildTextFromRRange(regValues, firstRangeStart, 10);   // Line 1
        String text2 = buildTextFromRRange(regValues, secondRangeStart, 10);  // Line 2
        
        char line1[200] = "";
        char line2[200] = "";
        char line3[50] = "";
        
        bool line1Empty = (text1.length() == 0);
        bool line2Empty = (text2.length() == 0);
        
        // Display logic: 2 lines / 1 line (line 1 empty) / 1 line (line 2 empty) / FUNCTIA ?
        if (line1Empty && line2Empty) {
          // Both lines are goale -> FUNCTIA ? pe a single line
          strcpy(line1, "Denumire functie");
          line2[0] = '\0';
        } else if (line1Empty) {
          // Line 1 is empty -> show only line 2
          line1[0] = '\0';
          text2.toCharArray(line2, 200);
        } else if (line2Empty) {
          // Line 2 is empty -> show only line 1
          text1.toCharArray(line1, 200);
          line2[0] = '\0';
        } else {
          // Both lines au characters -> show both
          text1.toCharArray(line1, 200);
          text2.toCharArray(line2, 200);
        }
        
        // Check if are shown both lines or only one/"Denumire function"
        bool bothLinesDisplayed = !line1Empty && !line2Empty;
        bool singleLineOrFunctionDisplayed = (line1Empty && line2Empty) || (line1Empty && !line2Empty) || (!line1Empty && line2Empty);
        
        // Variabile for line 3: font size and position Y
        int line3FontSize = 1;
        int line3YPosition = 22;
        
        // Build line 3 with "x:x:x" (D49:D48:D47) according requirements
        if (regValues->D_valid[4] && regValues->D_valid[3] && regValues->D_valid[2]) {
          uint16_t d49 = regValues->D49;
          uint16_t d48 = regValues->D48;
          uint16_t d47 = regValues->D47;
          
          if (d49 > 0) {
            // D49 > 0 -> show always D49:D48:D47 (hour:minut:second, x:x:x)
            char d49Str[10], d48Str[10], d47Str[10];
            formatTimeValue(d49, d49Str);
            strcat(line3, d49Str);
            
            // D48 are shown always when D49 > 0 (with "00" if D48 = 0)
            strcat(line3, ":");
            if (d48 == 0) {
              strcat(line3, "00");
            } else {
              formatTimeValue(d48, d48Str);
              strcat(line3, d48Str);
            }
            
            // D47 are shown always when D49 > 0 (with "00" if D47 = 0)
            strcat(line3, ":");
            if (d47 == 0) {
              strcat(line3, "00");
            } else {
              formatTimeValue(d47, d47Str);
              strcat(line3, d47Str);
            }
            
            // Set font size and Y based on caz
            if (bothLinesDisplayed) {
              // Case 1: both lines shown -> font size 1, Y = 22
              line3FontSize = 1;
              line3YPosition = 22;
            } else if (singleLineOrFunctionDisplayed) {
              // Caz 2: only o line or "Denumire function" -> font size 1, Y = 19
              line3FontSize = 1;
              line3YPosition = 19;
            }
          } else if (d48 > 0) {
            // D49 = 0, D48 > 0 -> show D48:D47
            char d48Str[10], d47Str[10];
            formatTimeValue(d48, d48Str);
            strcat(line3, d48Str);
            
            // D47 are shown always when D48 > 0 (with "00" if D47 = 0)
            strcat(line3, ":");
            if (d47 == 0) {
              strcat(line3, "00");
            } else {
              formatTimeValue(d47, d47Str);
              strcat(line3, d47Str);
            }
            
            // Set font size and Y based on caz
            if (bothLinesDisplayed) {
              // Case 1: both lines shown -> font size 1, Y = 22
              line3FontSize = 1;
              line3YPosition = 22;
            } else if (singleLineOrFunctionDisplayed) {
              // Caz 2: only o line or "Denumire function" -> font size 2, Y = 16
              line3FontSize = 2;
              line3YPosition = 16;
            }
          } else if (d47 > 0) {
            // D49 = 0, D48 = 0, D47 > 0 -> show only D47
            char d47Str[10];
            formatTimeValue(d47, d47Str);
            strcat(line3, d47Str);
            
            // Set font size and Y based on caz
            if (bothLinesDisplayed) {
              // Case 1: both lines shown -> font size 1, Y = 22
              line3FontSize = 1;
              line3YPosition = 22;
            } else if (singleLineOrFunctionDisplayed) {
              // Caz 2: only o line or "Denumire function" -> font size 2, Y = 16
              line3FontSize = 2;
              line3YPosition = 16;
            }
          }
        }
        
        // Calculate widths textului
        int line1Width_sc3 = strlen(line1) > 0 ? calcTextWidth(line1) : 0;
        int line2Width_sc3 = strlen(line2) > 0 ? calcTextWidth(line2) : 0;
        int line3Width_sc3 = strlen(line3) > 0 ? calcTextWidth(line3) : 0;
        
        // Recalculate width liniei 3 based on font size
        if (strlen(line3) > 0) {
          int16_t x, y;
          uint16_t w, h;
          dma_display->setTextSize(line3FontSize);
          dma_display->getTextBounds(line3, 0, 0, &x, &y, &w, &h);
          line3Width_sc3 = w;  // Exact width for the selected font size
          
          // If font size 2, reduce calculated width with reduced spacing (1px per character instead of 2px)
          if (line3FontSize == 2 && strlen(line3) > 1) {
            line3Width_sc3 = line3Width_sc3 - (strlen(line3) - 1);  // Reduce by (n-1) pixels for reduced spacing
          }
          
          dma_display->setTextSize(1);  // Reset to normal text size
        }
        
        // Translated comment in simple English
        // For "Denumire function" always force scrolling
        bool line1NeedsScroll = (strlen(line1) > 10) || (line1Empty && line2Empty && strcmp(line1, "Denumire functie") == 0);
        bool line2NeedsScroll = (strlen(line2) > 10);
        
        // If display-ul not era active or if both lines need scroll and not are synced
        if (!displayActive) {
          displayActive = true;
          scrollX_line1_scenario3 = dma_display->width();
          scrollX_line2_scenario3 = dma_display->width();
        } else if (line1NeedsScroll && line2NeedsScroll) {
          // If both need scrolling, sync them to start together
          // Check if one of ele s-a resetat recently or if are desincronizate
          // If both are in proces of scroll, le sync at cea that is more in spate
          if (scrollX_line1_scenario3 == dma_display->width() || scrollX_line2_scenario3 == dma_display->width()) {
            // If one resets, also reset the other one
            scrollX_line1_scenario3 = dma_display->width();
            scrollX_line2_scenario3 = dma_display->width();
          }
        }
        
        // Update scroll-urile if is necesar
        unsigned long currentTime = millis();
        if (currentTime - lastScrollUpdate >= SCROLL_UPDATE_INTERVAL) {
          if (line1NeedsScroll && line2NeedsScroll) {
            // If both need scroll, use same offset for both (synced)
            scrollX_line1_scenario3--;
            scrollX_line2_scenario3 = scrollX_line1_scenario3; // Sync at same position
            
            // Reset both when both lines finishes complet scroll-ul (all charactersle pass after x1)
            // Line a passed complet when: scrollX + lineWidth < 0
            if (scrollX_line1_scenario3 + line1Width_sc3 < 0 && scrollX_line2_scenario3 + line2Width_sc3 < 0) {
              // Both lines au terminat complet scroll-ul, reset to start again
              scrollX_line1_scenario3 = dma_display->width();
              scrollX_line2_scenario3 = dma_display->width();
            }
          } else {
            // If only one need scroll, handle it independently
            if (line1NeedsScroll) {
              scrollX_line1_scenario3--;
              // Reset when all characters have passed across the screen (after x1)
              if (scrollX_line1_scenario3 + line1Width_sc3 < 0) {
                scrollX_line1_scenario3 = dma_display->width();
              }
            }
            if (line2NeedsScroll) {
              scrollX_line2_scenario3--;
              // Reset when all characters have passed across the screen (after x1)
              if (scrollX_line2_scenario3 + line2Width_sc3 < 0) {
                scrollX_line2_scenario3 = dma_display->width();
              }
            }
          }
          lastScrollUpdate = currentTime;
        }
        
        // Calculate positions X for centrare (or scroll)
        int posX_line1, posX_line2;
        if (line1NeedsScroll && strlen(line1) > 0) {
          posX_line1 = scrollX_line1_scenario3;
        } else {
          posX_line1 = (dma_display->width() - line1Width_sc3) / 2;
        }
        
        if (line2NeedsScroll && strlen(line2) > 0) {
          posX_line2 = scrollX_line2_scenario3;
        } else {
          posX_line2 = (dma_display->width() - line2Width_sc3) / 2;
        }
        
        int centerX_line3 = (dma_display->width() - line3Width_sc3) / 2;
        
        // Determine positions Y based on ce lines are shown
        int yLine1, yLine2;
        
        if (line1Empty && line2Empty) {
          // "Denumire function" and line 3
          yLine1 = 5;  // "Denumire function" centered vertical
        } else if (line1Empty) {
          // Only line 2 and line 3
          yLine2 = 5;  // Line 2 centered vertical
        } else if (line2Empty) {
          // Only line 1 and line 3
          yLine1 = 5;  // Line 1 centered vertical
        } else {
          // All cele 3 lines
          yLine1 = 2;
          yLine2 = 12;
        }
        
        // Position Y for line 3 is set based on format (line3YPosition)
        
        // Show textul
        dma_display->fillScreen(0);
        dma_display->setTextSize(1);
        
        // Scenario 3: Line 3 are culoarea (128, 128, 255), lines 1, 2 and "FUNCTIA ?" au culori diferite based on M71-M79
        uint16_t colorLine3_sc3 = dma_display->color565(128, 128, 255);  // Line 3: albastru
        uint16_t colorLines12_sc3 = getColorForMIndex(activeMIndex);     // Lines 1, 2 and "FUNCTIA ?": culoare based on M index
        
        // Line 1 (if present)
        if (strlen(line1) > 0) {
          dma_display->setTextColor(colorLines12_sc3);
          dma_display->setCursor(posX_line1, yLine1);
          dma_display->print(line1);
        }
        
        // Line 2 (if present)
        if (strlen(line2) > 0) {
          dma_display->setTextColor(colorLines12_sc3);
          dma_display->setCursor(posX_line2, yLine2);
          dma_display->print(line2);
        }
        
        // Line 3 (x:x:x) if present - with font size and Y position based on requirements
        if (strlen(line3) > 0) {
          dma_display->setTextColor(colorLine3_sc3);
          dma_display->setCursor(centerX_line3, line3YPosition);
          
          if (line3FontSize == 2) {
            // Font size 2: draw each character individually with reduced spacing
            dma_display->setTextSize(2);
            for (int i = 0; i < strlen(line3); i++) {
              dma_display->print(line3[i]);
              // Reduce spacing-ul between characters (from 2px at 1px for text size 2)
              if (i < strlen(line3) - 1) {
                int16_t x, y;
                uint16_t w, h;
                dma_display->getTextBounds(String(line3[i]), 0, 0, &x, &y, &w, &h);
                // Adjust cursorul for spacing reduced (1px instead of 2px)
                dma_display->setCursor(dma_display->getCursorX() - 1, dma_display->getCursorY());
              }
            }
            dma_display->setTextSize(1);  // Reset to normal text size
          } else {
            // Font size 1: show normal
            dma_display->setTextSize(1);
            dma_display->print(line3);
          }
        }
      } else if (shouldDisplayPause) {
        // Scenario 4: M97=1 and D0>0 → display "PAUZA" with time
        const char *line1_pause = "PAUZA";
        char line2_pause[50] = "";
        
        // Variabile for line 2: font size and position Y
        int line2FontSize_pause = 1;
        int line2YPosition_pause = 20;
        
        // Build line 2 with time from D59:D58:D57 according requirements
        if (regValues->D_valid[7] && regValues->D_valid[6] && regValues->D_valid[5]) {
          uint16_t d59 = regValues->D59;
          uint16_t d58 = regValues->D58;
          uint16_t d57 = regValues->D57;
          
          if (d59 > 0) {
            // D59 > 0 -> show always D59:D58:D57 (hour:minut:second, x:x:x)
            char d59Str[10], d58Str[10], d57Str[10];
            formatTimeValue(d59, d59Str);
            strcat(line2_pause, d59Str);
            
            // D58 are shown always when D59 > 0 (with "00" if D58 = 0)
            strcat(line2_pause, ":");
            if (d58 == 0) {
              strcat(line2_pause, "00");
            } else {
              formatTimeValue(d58, d58Str);
              strcat(line2_pause, d58Str);
            }
            
            // D57 are shown always when D59 > 0 (with "00" if D57 = 0)
            strcat(line2_pause, ":");
            if (d57 == 0) {
              strcat(line2_pause, "00");
            } else {
              formatTimeValue(d57, d57Str);
              strcat(line2_pause, d57Str);
            }
            
            // Font size 1, Y = 20
            line2FontSize_pause = 1;
            line2YPosition_pause = 20;
          } else if (d58 > 0) {
            // D59 = 0, D58 > 0 -> show D58:D47
            char d58Str[10], d57Str[10];
            formatTimeValue(d58, d58Str);
            strcat(line2_pause, d58Str);
            
            // D57 are shown always when D58 > 0 (with "00" if D57 = 0)
            strcat(line2_pause, ":");
            if (d57 == 0) {
              strcat(line2_pause, "00");
            } else {
              formatTimeValue(d57, d57Str);
              strcat(line2_pause, d57Str);
            }
            
            // Font size 2, Y = 18
            line2FontSize_pause = 2;
            line2YPosition_pause = 18;
          } else if (d57 > 0) {
            // D59 = 0, D58 = 0, D57 > 0 -> show only D57
            char d57Str[10];
            formatTimeValue(d57, d57Str);
            strcat(line2_pause, d57Str);
            
            // Font size 2, Y = 19
            line2FontSize_pause = 2;
            line2YPosition_pause = 18;
          }
        }
        
        // Calculate widths textului
        int line1Width_pause = calcTextWidth(line1_pause);
        int line2Width_pause = strlen(line2_pause) > 0 ? calcTextWidth(line2_pause) : 0;
        
        // Recalculate width liniei 1 for text size 2 (font more large)
        int16_t x1, y1;
        uint16_t w1, h1;
        dma_display->setTextSize(2);
        dma_display->getTextBounds(line1_pause, 0, 0, &x1, &y1, &w1, &h1);
        line1Width_pause = w1;  // Exact width for text size 2
        dma_display->setTextSize(1);  // Reset to normal text size
        
        // Recalculate width liniei 2 based on font size
        if (strlen(line2_pause) > 0) {
          int16_t x, y;
          uint16_t w, h;
          dma_display->setTextSize(line2FontSize_pause);
          dma_display->getTextBounds(line2_pause, 0, 0, &x, &y, &w, &h);
          line2Width_pause = w;  // Exact width for the selected font size
          
          // If font size 2, reduce calculated width with reduced spacing (1px per character instead of 2px)
          if (line2FontSize_pause == 2 && strlen(line2_pause) > 1) {
            line2Width_pause = line2Width_pause - (strlen(line2_pause) - 1);  // Reduce by (n-1) pixels for reduced spacing
          }
          
          dma_display->setTextSize(1);  // Reset to normal text size
        }
        
        // If display-ul not era active, reset
        if (!displayActive) {
          displayActive = true;
        }
        
        // Calculate positions X for centrare
        int centerX_line1_pause = (dma_display->width() - line1Width_pause) / 2;
        int centerX_line2_pause = (dma_display->width() - line2Width_pause) / 2;
        
        // Show textul
        dma_display->fillScreen(0);
        dma_display->setTextSize(1);
        
        // Scenario 4: Line 1 (255, 128, 0) - portocaliu, Line 2 (128, 128, 255) - albastru
        // Line 1: "PAUZA" centered - with font more large
        dma_display->setTextColor(dma_display->color565(255, 128, 0));  // Portocaliu
        dma_display->setCursor(centerX_line1_pause, 1);  // Ajustat for text size 2
        dma_display->setTextSize(2);  // Font more large
        dma_display->print(line1_pause);
        dma_display->setTextSize(1);  // Reset to normal text size
        
        // Line 2: time (x:x:x or x:x or x) centered - with font size and Y position based on requirements
        if (strlen(line2_pause) > 0) {
          dma_display->setTextColor(dma_display->color565(128, 128, 255));  // Albastru
          dma_display->setCursor(centerX_line2_pause, line2YPosition_pause);
          
          if (line2FontSize_pause == 2) {
            // Font size 2: draw each character individually with reduced spacing
            dma_display->setTextSize(2);
            for (int i = 0; i < strlen(line2_pause); i++) {
              dma_display->print(line2_pause[i]);
              // Reduce spacing-ul between characters (from 2px at 1px for text size 2)
              if (i < strlen(line2_pause) - 1) {
                int16_t x, y;
                uint16_t w, h;
                dma_display->getTextBounds(String(line2_pause[i]), 0, 0, &x, &y, &w, &h);
                // Adjust cursorul for spacing reduced (1px instead of 2px)
                dma_display->setCursor(dma_display->getCursorX() - 1, dma_display->getCursorY());
              }
            }
            dma_display->setTextSize(1);  // Reset to normal text size
          } else {
            // Font size 1: show normal
            dma_display->setTextSize(1);
            dma_display->print(line2_pause);
          }
        }
      } else if (shouldDisplayPauseFinished) {
        // Scenario 5: M99=1 and D0>0 → display "PAUZA terminata" with time
        const char *line1_pause_finished = "PAUZA";
        const char *line2_pause_finished = "terminata";
        char line3_pause_finished[50] = "";
        
        // Variabile for line 3: font size and position Y
        int line3FontSize_pause_finished = 1;
        int line3YPosition_pause_finished = 20;
        
        // Build line 3 with time from D49:D48:D47 according requirements
        if (regValues->D_valid[4] && regValues->D_valid[3] && regValues->D_valid[2]) {
          uint16_t d49 = regValues->D49;
          uint16_t d48 = regValues->D48;
          uint16_t d47 = regValues->D47;
          
          if (d49 > 0) {
            // D49 > 0 -> show always D49:D48:D47 (hour:minut:second, x:x:x)
            char d49Str[10], d48Str[10], d47Str[10];
            formatTimeValue(d49, d49Str);
            strcat(line3_pause_finished, d49Str);
            
            // D48 are shown always when D49 > 0 (with "00" if D48 = 0)
            strcat(line3_pause_finished, ":");
            if (d48 == 0) {
              strcat(line3_pause_finished, "00");
            } else {
              formatTimeValue(d48, d48Str);
              strcat(line3_pause_finished, d48Str);
            }
            
            // D47 are shown always when D49 > 0 (with "00" if D47 = 0)
            strcat(line3_pause_finished, ":");
            if (d47 == 0) {
              strcat(line3_pause_finished, "00");
            } else {
              formatTimeValue(d47, d47Str);
              strcat(line3_pause_finished, d47Str);
            }
            
            // Font size 1, Y = 20
            line3FontSize_pause_finished = 1;
            line3YPosition_pause_finished = 20;
          } else if (d48 > 0) {
            // D49 = 0, D48 > 0 -> show D48:D47
            char d48Str[10], d47Str[10];
            formatTimeValue(d48, d48Str);
            strcat(line3_pause_finished, d48Str);
            
            // D47 are shown always when D48 > 0 (with "00" if D47 = 0)
            strcat(line3_pause_finished, ":");
            if (d47 == 0) {
              strcat(line3_pause_finished, "00");
            } else {
              formatTimeValue(d47, d47Str);
              strcat(line3_pause_finished, d47Str);
            }
            
            // Font size 2, Y = 18
            line3FontSize_pause_finished = 2;
            line3YPosition_pause_finished = 18;
          } else if (d47 > 0) {
            // D49 = 0, D48 = 0, D47 > 0 -> show only D47
            char d47Str[10];
            formatTimeValue(d47, d47Str);
            strcat(line3_pause_finished, d47Str);
            
            // Font size 2, Y = 19
            line3FontSize_pause_finished = 2;
            line3YPosition_pause_finished = 18;
          }
        }
        
        // Calculate widths textului
        int line1Width_pause_finished = calcTextWidth(line1_pause_finished);
        int line2Width_pause_finished = calcTextWidth(line2_pause_finished);
        int line3Width_pause_finished = strlen(line3_pause_finished) > 0 ? calcTextWidth(line3_pause_finished) : 0;
        
        // Recalculate width liniei 3 based on font size
        if (strlen(line3_pause_finished) > 0) {
          int16_t x, y;
          uint16_t w, h;
          dma_display->setTextSize(line3FontSize_pause_finished);
          dma_display->getTextBounds(line3_pause_finished, 0, 0, &x, &y, &w, &h);
          line3Width_pause_finished = w;  // Exact width for the selected font size
          
          // If font size 2, reduce calculated width with reduced spacing (1px per character instead of 2px)
          if (line3FontSize_pause_finished == 2 && strlen(line3_pause_finished) > 1) {
            line3Width_pause_finished = line3Width_pause_finished - (strlen(line3_pause_finished) - 1);  // Reduce by (n-1) pixels for reduced spacing
          }
          
          dma_display->setTextSize(1);  // Reset to normal text size
        }
        
        // If display-ul not era active, reset
        if (!displayActive) {
          displayActive = true;
        }
        
        // Calculate positions X for centrare (all lines are statice and centered)
        int centerX_line1_pause_finished = (dma_display->width() - line1Width_pause_finished) / 2;
        int centerX_line2_pause_finished = (dma_display->width() - line2Width_pause_finished) / 2;
        int centerX_line3_pause_finished = (dma_display->width() - line3Width_pause_finished) / 2;
        
        // Show textul
        dma_display->fillScreen(0);
        dma_display->setTextSize(1);
        
        // Scenario 5: Line 1 and 2 (255, 128, 128) - roz, Line 3 (128, 128, 255) - albastru
        // Line 1: "PAUZA" static and centered
        dma_display->setTextColor(dma_display->color565(255, 128, 128));  // Roz
        dma_display->setCursor(centerX_line1_pause_finished, 0);
        dma_display->print(line1_pause_finished);
        
        // Line 2: "terminata" static and centered
        dma_display->setTextColor(dma_display->color565(255, 128, 128));  // Roz
        dma_display->setCursor(centerX_line2_pause_finished, 9);
        dma_display->print(line2_pause_finished);
        
        // Line 3: time (x:x:x or x:x or x) static and centered - with font size and Y position based on requirements
        if (strlen(line3_pause_finished) > 0) {
          dma_display->setTextColor(dma_display->color565(128, 128, 255));  // Albastru
          dma_display->setCursor(centerX_line3_pause_finished, line3YPosition_pause_finished);
          
          if (line3FontSize_pause_finished == 2) {
            // Font size 2: draw each character individually with reduced spacing
            dma_display->setTextSize(2);
            for (int i = 0; i < strlen(line3_pause_finished); i++) {
              dma_display->print(line3_pause_finished[i]);
              // Reduce spacing-ul between characters (from 2px at 1px for text size 2)
              if (i < strlen(line3_pause_finished) - 1) {
                int16_t x, y;
                uint16_t w, h;
                dma_display->getTextBounds(String(line3_pause_finished[i]), 0, 0, &x, &y, &w, &h);
                // Adjust cursorul for spacing reduced (1px instead of 2px)
                dma_display->setCursor(dma_display->getCursorX() - 1, dma_display->getCursorY());
              }
            }
            dma_display->setTextSize(1);  // Reset to normal text size
          } else {
            // Font size 1: show normal
            dma_display->setTextSize(1);
            dma_display->print(line3_pause_finished);
          }
        }
      } else if (shouldDisplayUnavailable) {
        // Scenario 7: M120=0 -> show "Acest post is INDISPONIBIL E1"
        const char *line1_unavailable = "Acest post";
        const char *line2_unavailable = "este";
        const char *line3_unavailable = "INDISPONIBIL E1";
        
        // Calculate widths textului
        int line1Width_unavailable = calcTextWidth(line1_unavailable);
        int line2Width_unavailable = calcTextWidth(line2_unavailable);
        int line3Width_unavailable = calcTextWidth(line3_unavailable);
        
        // If display-ul not era active, reset scroll-ul for line 3
        if (!displayActive) {
          displayActive = true;
          scrollX_line3_scenario7 = dma_display->width();
        }
        
        // Calculate positions X for centrare (lines 1 and 2 are statice, line 3 are scroll)
        int centerX_line1_unavailable = (dma_display->width() - line1Width_unavailable) / 2;
        int centerX_line2_unavailable = (dma_display->width() - line2Width_unavailable) / 2;
        int posX_line3_unavailable = scrollX_line3_scenario7;
        
        // Update scroll-ul for line 3
        if (currentTime - lastScrollUpdate >= SCROLL_UPDATE_INTERVAL) {
          lastScrollUpdate = currentTime;
          scrollX_line3_scenario7--;
          // Reset when all characters have passed across the screen (after x1)
          if (scrollX_line3_scenario7 + line3Width_unavailable < 0) {
            // All characters have passed across the screen, reset to start again
            scrollX_line3_scenario7 = dma_display->width();
          }
        }
        
        // Show textul
        dma_display->fillScreen(0);
        // Scenario 7: (255, 0, 0) - red
        dma_display->setTextColor(dma_display->color565(255, 0, 0));
        dma_display->setTextSize(1);
        
        // Line 1: "Acest post" static and centered
        dma_display->setCursor(centerX_line1_unavailable, 2);
        dma_display->print(line1_unavailable);
        
        // Line 2: "is" static and centered
        dma_display->setCursor(centerX_line2_unavailable, 12);
        dma_display->print(line2_unavailable);
        
        // Line 3: "INDISPONIBIL E1" with scroll from right to left
        dma_display->setCursor(posX_line3_unavailable, 22);
        dma_display->print(line3_unavailable);
      } else if (shouldDisplayCommError) {
        // Scenario 6: PLC communication error → display "Acest post is INDISPONIBIL E0"
        const char *line1_comm_error = "Acest post";
        const char *line2_comm_error = "este";
        const char *line3_comm_error = "INDISPONIBIL E0";
        
        // Calculate widths textului
        int line1Width_comm_error = calcTextWidth(line1_comm_error);
        int line2Width_comm_error = calcTextWidth(line2_comm_error);
        int line3Width_comm_error = calcTextWidth(line3_comm_error);
        
        // If display-ul not era active, reset scroll-ul for line 3
        if (!displayActive) {
          displayActive = true;
          scrollX_line3_scenario6 = dma_display->width();
        }
        
        // Calculate positions X for centrare (lines 1 and 2 are statice, line 3 are scroll)
        int centerX_line1_comm_error = (dma_display->width() - line1Width_comm_error) / 2;
        int centerX_line2_comm_error = (dma_display->width() - line2Width_comm_error) / 2;
        int posX_line3_comm_error = scrollX_line3_scenario6;
        
        // Update scroll-ul for line 3
        if (currentTime - lastScrollUpdate >= SCROLL_UPDATE_INTERVAL) {
          lastScrollUpdate = currentTime;
          scrollX_line3_scenario6--;
          // Reset when all characters have passed across the screen (after x1)
          if (scrollX_line3_scenario6 + line3Width_comm_error < 0) {
            // All characters have passed across the screen, reset to start again
            scrollX_line3_scenario6 = dma_display->width();
          }
        }
        
        // Show textul
        dma_display->fillScreen(0);
        // Scenario 6: (255, 0, 0) - red
        dma_display->setTextColor(dma_display->color565(255, 0, 0));
        dma_display->setTextSize(1);
        
        // Line 1: "Acest post" static and centered
        dma_display->setCursor(centerX_line1_comm_error, 2);
        dma_display->print(line1_comm_error);
        
        // Line 2: "is" static and centered
        dma_display->setCursor(centerX_line2_comm_error, 12);
        dma_display->print(line2_comm_error);
        
        // Line 3: "INDISPONIBIL E0" with scroll from right to left
        dma_display->setCursor(posX_line3_comm_error, 22);
        dma_display->print(line3_comm_error);
      } else {
        // If conditions are not met, clear the screen
        if (displayActive) {
          dma_display->fillScreen(0);
          displayActive = false;
          scrollX = dma_display->width(); // Reset scroll for scenario 1
          scrollX_line1_scenario3 = dma_display->width(); // Reset scroll for scenario 3
          scrollX_line2_scenario3 = dma_display->width(); // Reset scroll for scenario 3
          scrollX_scenario5 = dma_display->width(); // Reset scroll for scenario 5
          scrollX_line3_scenario12 = dma_display->width(); // Reset scroll for scenario 1.2
          scrollX_line3_scenario6 = dma_display->width(); // Reset scroll for scenario 6 (PLC communication error)
          scrollX_line3_scenario7 = dma_display->width(); // Reset scroll for scenario 7 (M120=0 unavailable)
        }
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(1));  // Yield control to FreeRTOS
  }
}

void setup() {
  // Initialization Serial Monitor
  Serial.begin(115200);
  while (!Serial && millis() < 5000) {
    delay(10);  // Wait up to 5 seconds for Serial
  }
  
  // Create task for Modbus communication on Core 0
  xTaskCreatePinnedToCore(
    taskModbusCommunication,    // Task function
    "ModbusComm",               // Numele task-ului
    8192,                       // Stack size (bytes)
    NULL,                       // Parametri task
    1,                          // Prioritate (1 = normal)
    NULL,                       // Task handle (not avem nevoie)
    0                           // Core 0
  );
  
  // Create task for LED matrix display on Core 1
  xTaskCreatePinnedToCore(
    taskMatrixDisplay,          // Task function
    "MatrixDisplay",            // Numele task-ului
    8192,                       // Stack size (bytes)
    NULL,                       // Parametri task
    1,                          // Prioritate (1 = normal)
    NULL,                       // Task handle (not avem nevoie)
    1                           // Core 1
  );
  
  // Setup ends, but tasks continue to run
}

void loop() {
  // Main loop remains empty, everything runs in tasks
  vTaskDelay(pdMS_TO_TICKS(1000));  // Delay lung, tasks handle everything
}
