/*
 * File: FATEKRS232.cpp
 * Description: RS232 ASCII communication implementation for reading FATEK PLC registers.
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

#include "FATEKRS232.h"
#include "LRC_checksum.h"

// Define pins ESP32 (can be overridden from main.ino if defined there)
#ifndef MODBUS_RX_PIN
#define MODBUS_RX_PIN 16
#endif
#ifndef MODBUS_TX_PIN
#define MODBUS_TX_PIN 17
#endif
#ifndef MODBUS_BAUDRATE
#define MODBUS_BAUDRATE 115200
#endif
#ifndef MODBUS_SLAVE_ID
#define MODBUS_SLAVE_ID 1
#endif

// Configuration RS232 ASCII FATEK
#define STATION_NO "01"  // Station number (poate fi configurat from MODBUS_SLAVE_ID)
#define TIMEOUT_MS 2000
#define READ_INTERVAL_MS 10  // Interval between reads (similar to readInterval from Modbus)

// Response structure FATEK RS232
struct FatekResponse {
    bool success;
    String rawData; 
    String errorMsg;
};

// Variabile globale
RegisterValues regValues;
unsigned long lastReadTime = 0;
ReadState currentState = READ_M71_M79;

// Timestamp for last valid communication
unsigned long lastSuccessfulCommTime = 0;
const unsigned long COMM_TIMEOUT = 3000; // 3 seconds communication timeout

// Display control Serial Monitor
bool serialMonitorEnabled = true;  // Default: enabled

// Slave address (converted from MODBUS_SLAVE_ID)
String stationNo = STATION_NO;

// Prototypes for internal functions
FatekResponse fatekReadRegisters(char type, int startAddr, int count);
FatekResponse fatekReadBits(char type, int startAddr, int count);
FatekResponse sendFatekCommand(String commandCode, String body);

// Translated comment in simple English

FatekResponse fatekReadBits(char type, int startAddr, int count) {
    char countBuf[3];
    sprintf(countBuf, "%02d", count);
    char addrBuf[5];
    sprintf(addrBuf, "%04d", startAddr); 
    
    String body = String(countBuf) + String(type) + String(addrBuf);
    return sendFatekCommand("44", body);
}

FatekResponse fatekReadRegisters(char type, int startAddr, int count) {
    char countBuf[3];
    sprintf(countBuf, "%02d", count);
    char addrBuf[6];
    sprintf(addrBuf, "%05d", startAddr); 
    
    String body = String(countBuf) + String(type) + String(addrBuf);
    return sendFatekCommand("46", body);
}

FatekResponse sendFatekCommand(String commandCode, String body) {
    FatekResponse result;
    result.success = false;

    String payload = stationNo + commandCode + body;
    String lrc = LRC_calc(payload);
    String fullCommand = "\x02" + payload + lrc + "\x03";

    while(Serial2.available()) Serial2.read(); 
    Serial2.print(fullCommand);

    String response = "";
    unsigned long startTime = millis();
    bool etxFound = false;

    while (millis() - startTime < TIMEOUT_MS) {
        if (Serial2.available()) {
            char c = Serial2.read();
            response += c;
            if (c == 0x03) { etxFound = true; break; }
        }
    }

    if (!etxFound) { 
        result.errorMsg = "Timeout"; 
        return result; 
    }
    if (response.length() < 9) { 
        result.errorMsg = "Incomplete"; 
        return result; 
    }

    char errorCode = response.charAt(5);
    if (errorCode != '0') {
        result.errorMsg = String("PLC Err: ") + errorCode;
        return result;
    }

    if (response.length() > 9) {
        result.rawData = response.substring(6, response.length() - 3);
    } else {
        result.rawData = "";
    }

    result.success = true;
    return result;
}

// ================= FUNCTII PROCESARE DATE =================

void processBitsResponse(FatekResponse resp, ReadState state) {
    if (!resp.success) {
        return;
    }
    
    // Update timestamp of the last valid communication
    lastSuccessfulCommTime = millis();
    
    // Process response based on current state
    if (state == READ_M71_M79) {
        // Read 9 bits (M71-M79)
        if (resp.rawData.length() >= 9) {
            regValues.M71 = (resp.rawData[0] == '1');
            regValues.M72 = (resp.rawData[1] == '1');
            regValues.M73 = (resp.rawData[2] == '1');
            regValues.M74 = (resp.rawData[3] == '1');
            regValues.M75 = (resp.rawData[4] == '1');
            regValues.M76 = (resp.rawData[5] == '1');
            regValues.M77 = (resp.rawData[6] == '1');
            regValues.M78 = (resp.rawData[7] == '1');
            regValues.M79 = (resp.rawData[8] == '1');
            for (int i = 0; i < 9; i++) regValues.M_valid[i] = true;
        }
    } else if (state == READ_M94_M99) {
        // Read 6 bits (M94-M99)
        if (resp.rawData.length() >= 6) {
            regValues.M94 = (resp.rawData[0] == '1');
            regValues.M95 = (resp.rawData[1] == '1');
            regValues.M96 = (resp.rawData[2] == '1');
            regValues.M97 = (resp.rawData[3] == '1');
            regValues.M98 = (resp.rawData[4] == '1');
            regValues.M99 = (resp.rawData[5] == '1');
            for (int i = 9; i < 15; i++) regValues.M_valid[i] = true;
        }
    } else if (state == READ_M120) {
        // Read 1 bit (M120)
        if (resp.rawData.length() >= 1) {
            regValues.M120 = (resp.rawData[0] == '1');
            regValues.M_valid[15] = true;
        }
    } else if (state == READ_M320_M322) {
        // Read 3 bits (M320-M322)
        if (resp.rawData.length() >= 3) {
            regValues.M320 = (resp.rawData[0] == '1');
            regValues.M321 = (resp.rawData[1] == '1');
            regValues.M322 = (resp.rawData[2] == '1');
            regValues.M_valid[16] = true;
            regValues.M_valid[17] = true;
            regValues.M_valid[18] = true;
        }
    }
}

void processRegistersResponse(FatekResponse resp, ReadState state) {
    if (!resp.success) {
        return;
    }
    
    // Update timestamp of the last valid communication
    lastSuccessfulCommTime = millis();
    
    // Process response based on current state
    if (state == READ_D0) {
        // Read 1 register (D0) - 4 characters HEX
        if (resp.rawData.length() >= 4) {
            String hexVal = resp.rawData.substring(0, 4);
            regValues.D0 = strtol(hexVal.c_str(), NULL, 16);
            regValues.D_valid[0] = true;
        }
    } else if (state == READ_D10) {
        // Read 1 register (D10) - 4 characters HEX
        if (resp.rawData.length() >= 4) {
            String hexVal = resp.rawData.substring(0, 4);
            regValues.D10 = strtol(hexVal.c_str(), NULL, 16);
            regValues.D_valid[1] = true;
        }
    } else if (state == READ_D47_D49) {
        // Read 3 registers (D47-D49) - 12 characters HEX (4 per register)
        if (resp.rawData.length() >= 12) {
            String hexVal1 = resp.rawData.substring(0, 4);
            String hexVal2 = resp.rawData.substring(4, 8);
            String hexVal3 = resp.rawData.substring(8, 12);
            regValues.D47 = strtol(hexVal1.c_str(), NULL, 16);
            regValues.D48 = strtol(hexVal2.c_str(), NULL, 16);
            regValues.D49 = strtol(hexVal3.c_str(), NULL, 16);
            regValues.D_valid[2] = true;
            regValues.D_valid[3] = true;
            regValues.D_valid[4] = true;
        }
    } else if (state == READ_D57_D59) {
        // Read 3 registers (D57-D59) - 12 characters HEX (4 per register)
        if (resp.rawData.length() >= 12) {
            String hexVal1 = resp.rawData.substring(0, 4);
            String hexVal2 = resp.rawData.substring(4, 8);
            String hexVal3 = resp.rawData.substring(8, 12);
            regValues.D57 = strtol(hexVal1.c_str(), NULL, 16);
            regValues.D58 = strtol(hexVal2.c_str(), NULL, 16);
            regValues.D59 = strtol(hexVal3.c_str(), NULL, 16);
            regValues.D_valid[5] = true;
            regValues.D_valid[6] = true;
            regValues.D_valid[7] = true;
        }
    } else if (state == READ_R10_R99) {
        // Read 90 registers (R10-R99) - 360 characters HEX (4 per register)
        if (resp.rawData.length() >= 360) {
            for (int i = 0; i < 90; i++) {
                String hexVal = resp.rawData.substring(i*4, (i+1)*4);
                regValues.R[i] = strtol(hexVal.c_str(), NULL, 16);
                regValues.R_valid[i] = true;
            }
        }
    } else if (state == READ_R110_R199) {
        // Read 90 registers (R110-R199) - 360 characters HEX (4 per register)
        if (resp.rawData.length() >= 360) {
            for (int i = 0; i < 90; i++) {
                String hexVal = resp.rawData.substring(i*4, (i+1)*4);
                regValues.R[90 + i] = strtol(hexVal.c_str(), NULL, 16);
                regValues.R_valid[90 + i] = true;
            }
        }
    }
}

// ================= FUNCTII PUBLICE (INTERFATA COMPATIBILA With FATEKModbus) =================

// Function to get a pointer to register values
RegisterValues* getRegisterValues() {
  return &regValues;
}

// Functions for display control Serial Monitor
void setSerialMonitorEnabled(bool enabled) {
  serialMonitorEnabled = enabled;
}

bool getSerialMonitorEnabled() {
  return serialMonitorEnabled;
}

// Function to convert register value to ASCII characters
String valueToASCII(uint16_t value) {
  if (value == 0) {
    return "0";
  }
  
  // Step 1: modulo 256 (low byte)
  uint8_t lowByte = value % 256;
  char lowChar = (lowByte >= 32 && lowByte <= 126) ? (char)lowByte : '?';
  
  // Step 2: division at 256 (high byte)
  uint8_t highByte = value / 256;
  
  // If high byte is 0, show only low byte
  if (highByte == 0) {
    return String(lowChar);
  }
  
  // Otherwise show both characters (low byte first, high byte second)
  char highChar = (highByte >= 32 && highByte <= 126) ? (char)highByte : '?';
  return String(lowChar) + String(highChar);
}

// Function to display register values on Serial Monitor
void displayRegisterValues() {
  // Check if Serial Monitor is enabled
  if (!serialMonitorEnabled) {
    return;  // Do not show anything if Serial Monitor is disabled
  }
  
  Serial.println("\n========================================");
  Serial.println("  VALORI CITITE - " + String(millis() / 1000.0, 2) + "s");
  Serial.println("========================================");
  
  // Show Discrete M Relays
  Serial.println("\n--- DISCRETE M RELAYS ---");
  
  Serial.print("M71-M79: ");
  bool allValid = true;
  for (int i = 0; i < 9; i++) {
    if (!regValues.M_valid[i]) {
      allValid = false;
      break;
    }
  }
  if (allValid) {
    Serial.println();
    Serial.println(" M71=" + String(regValues.M71 ? "ON" : "OFF"));
    Serial.println(" M72=" + String(regValues.M72 ? "ON" : "OFF"));
    Serial.println(" M73=" + String(regValues.M73 ? "ON" : "OFF"));
    Serial.println(" M74=" + String(regValues.M74 ? "ON" : "OFF"));
    Serial.println(" M75=" + String(regValues.M75 ? "ON" : "OFF"));
    Serial.println(" M76=" + String(regValues.M76 ? "ON" : "OFF"));
    Serial.println(" M77=" + String(regValues.M77 ? "ON" : "OFF"));
    Serial.println(" M78=" + String(regValues.M78 ? "ON" : "OFF"));
    Serial.println(" M79=" + String(regValues.M79 ? "ON" : "OFF"));
    Serial.println();
  } else {
    Serial.println("EROARE - valori nevalide");
  }
  
  if (regValues.M_valid[9] && regValues.M_valid[10] && regValues.M_valid[11] && 
      regValues.M_valid[12] && regValues.M_valid[13] && regValues.M_valid[14]) {
    Serial.println(" M94: " + String(regValues.M94 ? "ON" : "OFF"));
    Serial.println(" M95: " + String(regValues.M95 ? "ON" : "OFF"));
    Serial.println(" M96: " + String(regValues.M96 ? "ON" : "OFF"));
    Serial.println(" M97: " + String(regValues.M97 ? "ON" : "OFF"));
    Serial.println(" M98: " + String(regValues.M98 ? "ON" : "OFF"));
    Serial.println(" M99: " + String(regValues.M99 ? "ON" : "OFF"));
  } else {
    Serial.println("M94-M99: EROARE - valori nevalide");
  }
  
  if (regValues.M_valid[15]) {
    Serial.println(" M120: " + String(regValues.M120 ? "ON" : "OFF"));
  } else {
    Serial.println("M120: EROARE - valoare nevalidă");
  }
  
  if (regValues.M_valid[16] && regValues.M_valid[17] && regValues.M_valid[18]) {
    Serial.println(" M320: " + String(regValues.M320 ? "ON" : "OFF"));
    Serial.println(" M321: " + String(regValues.M321 ? "ON" : "OFF"));
    Serial.println(" M322: " + String(regValues.M322 ? "ON" : "OFF"));
  } else {
    Serial.println("M320-M322: EROARE - valori nevalide");
  }
  
  // Show Data Registers
  Serial.println("\n--- DATA REGISTERS (D) ---");
  
  if (regValues.D_valid[0]) {
    Serial.print(" D0: ");
    Serial.print(regValues.D0);
    Serial.print(" (0x");
    Serial.print(regValues.D0, HEX);
    Serial.println(")");
  } else {
    Serial.println("D0: EROARE - valoare nevalidă");
  }
  
  if (regValues.D_valid[1]) {
    Serial.print(" D10: ");
    Serial.print(regValues.D10);
    Serial.print(" (0x");
    Serial.print(regValues.D10, HEX);
    Serial.println(")");
  } else {
    Serial.println("D10: EROARE - valoare nevalidă");
  }
  
  if (regValues.D_valid[2] && regValues.D_valid[3] && regValues.D_valid[4]) {
    Serial.print(" D47: ");
    Serial.print(regValues.D47);
    Serial.print(" (0x");
    Serial.print(regValues.D47, HEX);
    Serial.println(")");
    Serial.print(" D48: ");
    Serial.print(regValues.D48);
    Serial.print(" (0x");
    Serial.print(regValues.D48, HEX);
    Serial.println(")");
    Serial.print(" D49: ");
    Serial.print(regValues.D49);
    Serial.print(" (0x");
    Serial.print(regValues.D49, HEX);
    Serial.println(")");
  } else {
    Serial.println("D47-D49: EROARE - valori nevalide");
  }
  
  if (regValues.D_valid[5] && regValues.D_valid[6] && regValues.D_valid[7]) {
    Serial.print(" D57: ");
    Serial.print(regValues.D57);
    Serial.print(" (0x");
    Serial.print(regValues.D57, HEX);
    Serial.println(")");
    Serial.print(" D58: ");
    Serial.print(regValues.D58);
    Serial.print(" (0x");
    Serial.print(regValues.D58, HEX);
    Serial.println(")");
    Serial.print(" D59: ");
    Serial.print(regValues.D59);
    Serial.print(" (0x");
    Serial.print(regValues.D59, HEX);
    Serial.println(")");
  } else {
    Serial.println("D57-D59: EROARE - valori nevalide");
  }
  
  // Show Holding Registers
  Serial.println("\n--- HOLDING REGISTERS (R) ---");
  Serial.println("R10-R99, R110-R199:");
  
  bool hasErrors = false;
  
  // Show R10-R99 (90 registers, index 0-89)
  for (int i = 0; i < 90; i++) {
    int regNum = i + 10;
    if (regValues.R_valid[i]) {
      Serial.print(" R" + String(regNum) + ": ");
      Serial.print(regValues.R[i]);
      Serial.print(" (0x");
      Serial.print(regValues.R[i], HEX);
      Serial.print(") - ");
      Serial.println(valueToASCII(regValues.R[i]));
    } else {
      if (!hasErrors) {
        Serial.println("EROARE - unele valori nevalide:");
        hasErrors = true;
      }
      Serial.println(" R" + String(regNum) + ": EROARE");
    }
  }
  
  // Show R110-R199 (90 registers, index 90-179)
  for (int i = 0; i < 90; i++) {
    int regNum = i + 110;
    if (regValues.R_valid[90 + i]) {
      Serial.print(" R" + String(regNum) + ": ");
      Serial.print(regValues.R[90 + i]);
      Serial.print(" (0x");
      Serial.print(regValues.R[90 + i], HEX);
      Serial.print(") - ");
      Serial.println(valueToASCII(regValues.R[90 + i]));
    } else {
      if (!hasErrors) {
        Serial.println("EROARE - unele valori nevalide:");
        hasErrors = true;
      }
      Serial.println(" R" + String(regNum) + ": EROARE");
    }
  }
  
  Serial.println("========================================");
  Serial.println("========================================\n");
}

// Function to check if PLC communication is working
bool isCommunicationActive() {
  // Check if more than 3 seconds since last valid communication
  unsigned long currentTime = millis();
  
  // If there was never a valid communication and initialization is not done yet
  if (lastSuccessfulCommTime == 0) {
    // On first run, communication is not active until first response
    // But if initialization was done recently (< 3 seconds), allow time to connect
    if (currentTime < 3000) {
      return true; // Allow time for initialization
    }
    return false;
  }
  
  // Check if more than 3 seconds since last valid communication
  if (currentTime - lastSuccessfulCommTime > COMM_TIMEOUT) {
    return false; // Communication timed out
  }
  
  return true; // Communication active
}

// Function to get current slave ID
uint8_t getCurrentSlaveID() {
  return MODBUS_SLAVE_ID;
}

// Initialization
void initFATEKModbus() {
  // Convert MODBUS_SLAVE_ID to string for station number
  char stationBuf[3];
  sprintf(stationBuf, "%02d", MODBUS_SLAVE_ID);
  stationNo = String(stationBuf);
  
  // Initialization values
  for (int i = 0; i < 19; i++) {
    regValues.M_valid[i] = false;
  }
  
  // Communication timestamp initialization
  lastSuccessfulCommTime = 0; // Will be updated on first valid response
  regValues.M71 = regValues.M72 = regValues.M73 = regValues.M74 = regValues.M75 = false;
  regValues.M76 = regValues.M77 = regValues.M78 = regValues.M79 = false;
  regValues.M94 = regValues.M95 = regValues.M96 = regValues.M97 = regValues.M98 = regValues.M99 = false;
  regValues.M120 = false;
  regValues.M320 = regValues.M321 = regValues.M322 = false;
  
  for (int i = 0; i < 8; i++) {
    regValues.D_valid[i] = false;
  }
  regValues.D0 = 0;
  regValues.D10 = 0;
  regValues.D47 = regValues.D48 = regValues.D49 = 0;
  regValues.D57 = regValues.D58 = regValues.D59 = 0;
  
  for (int i = 0; i < 180; i++) {
    regValues.R[i] = 0;
    regValues.R_valid[i] = false;
  }
  
  // Initialize serial interface for RS232 (7E1 - 7 data bits, Even parity, 1 stop bit)
  Serial2.begin(MODBUS_BAUDRATE, SERIAL_7E1, MODBUS_RX_PIN, MODBUS_TX_PIN);
  delay(50);
  
  if (serialMonitorEnabled) {
    Serial.printf("Comunicare FATEK RS232 ASCII inițializată cu station number: %s\n", stationNo.c_str());
  }
  
  lastReadTime = millis();
}

// Loop principal
void loopFATEKModbus() {
  unsigned long currentTime = millis();
  
  // Check if time interval passed for next read
  if (currentTime - lastReadTime >= READ_INTERVAL_MS) {
    FatekResponse resp;
    
    // Build request based on current state
    switch (currentState) {
      case READ_M71_M79:
        // Read M71-M79 (9 bits)
        resp = fatekReadBits('M', 71, 9);
        processBitsResponse(resp, READ_M71_M79);
        break;
        
      case READ_M94_M99:
        // Read M94-M99 (6 bits)
        resp = fatekReadBits('M', 94, 6);
        processBitsResponse(resp, READ_M94_M99);
        break;
        
      case READ_M120:
        // Read M120 (1 bit)
        resp = fatekReadBits('M', 120, 1);
        processBitsResponse(resp, READ_M120);
        break;
        
      case READ_M320_M322:
        // Read M320-M322 (3 bits)
        resp = fatekReadBits('M', 320, 3);
        processBitsResponse(resp, READ_M320_M322);
        break;
        
      case READ_D0:
        // Read D0 (1 register)
        resp = fatekReadRegisters('D', 0, 1);
        processRegistersResponse(resp, READ_D0);
        break;
        
      case READ_D10:
        // Read D10 (1 register)
        resp = fatekReadRegisters('D', 10, 1);
        processRegistersResponse(resp, READ_D10);
        break;
        
      case READ_D47_D49:
        // Read D47-D49 (3 registers)
        resp = fatekReadRegisters('D', 47, 3);
        processRegistersResponse(resp, READ_D47_D49);
        break;
        
      case READ_D57_D59:
        // Read D57-D59 (3 registers)
        resp = fatekReadRegisters('D', 57, 3);
        processRegistersResponse(resp, READ_D57_D59);
        break;
        
      case READ_R10_R99:
        // Read R10-R99 (90 registers)
        resp = fatekReadRegisters('R', 10, 90);
        processRegistersResponse(resp, READ_R10_R99);
        break;
        
      case READ_R110_R199:
        // Read R110-R199 (90 registers)
        resp = fatekReadRegisters('R', 110, 90);
        processRegistersResponse(resp, READ_R110_R199);
        break;
    }
    
    // Move to next read
    switch (currentState) {
      case READ_M71_M79: currentState = READ_M94_M99; break;
      case READ_M94_M99: currentState = READ_M120; break;
      case READ_M120: currentState = READ_M320_M322; break;
      case READ_M320_M322: currentState = READ_D0; break;
      case READ_D0: currentState = READ_D10; break;
      case READ_D10: currentState = READ_D47_D49; break;
      case READ_D47_D49: currentState = READ_D57_D59; break;
      case READ_D57_D59: currentState = READ_R10_R99; break;
      case READ_R10_R99: currentState = READ_R110_R199; break;
      case READ_R110_R199: 
        currentState = READ_M71_M79;  // Return to start
        break;
    }
    
    // Delay between requests to allow PLC processing
    delay(20);
    
    lastReadTime = currentTime;
  }
  
  delay(10);
  yield();  // Allows other tasks (such as scroll animation) to run
}
