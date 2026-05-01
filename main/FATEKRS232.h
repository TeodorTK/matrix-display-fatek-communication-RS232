/*
 * File: FATEKRS232.h
 * Description: Public interface and data structures for FATEK RS232 communication.
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

#ifndef FATEK_RS232_H
#define FATEK_RS232_H

#include <Arduino.h>

// Structure for stocarea valueslor citite (same ca in FATEKModbus.h)
struct RegisterValues {
  // Discrete M Relays
  bool M71, M72, M73, M74, M75, M76, M77, M78, M79;  // M71-M79 (9 coils)
  bool M94, M95, M96, M97, M98, M99;                 // M94-M99 (6 coils)
  bool M120;                                           // M120 (1 coil)
  bool M320, M321, M322;                              // M320-M322 (3 coils)
  bool M_valid[19];  // Validare for all coils (9+6+1+3=19)
  
  // Data Registers (D)
  uint16_t D0;                                        // D0
  uint16_t D10;                                       // D10
  uint16_t D47, D48, D49;                            // D47-D49 (3 registers)
  uint16_t D57, D58, D59;                            // D57-D59 (3 registers)
  bool D_valid[8];  // Validare for all registrele D (1+1+3+3=8)
  
  // Holding Registers (R)
  uint16_t R[180];  // R10-R99 (90 registers) + R110-R199 (90 registers) = 180 registers (without R100-R109)
  bool R_valid[180];
};

// Index that tracks what is currently read
enum ReadState {
  READ_M71_M79,       // Read M71-M79 (9 coils)
  READ_M94_M99,       // Read M94-M99 (6 coils)
  READ_M120,          // Read M120 (1 coil)
  READ_M320_M322,     // Read M320-M322 (3 coils)
  READ_D0,            // Read D0 (1 register)
  READ_D10,           // Read D10 (1 register)
  READ_D47_D49,       // Read D47-D49 (3 registers)
  READ_D57_D59,       // Read D57-D59 (3 registers)
  READ_R10_R99,       // Read R10-R99 (90 registers)
  READ_R110_R199      // Read R110-R199 (90 registers)
};

// Function declarations (same interface ca FATEKModbus.h)
void initFATEKModbus();
void loopFATEKModbus();
RegisterValues* getRegisterValues();  // Function to get register values
void displayRegisterValues();  // Function to display register values on Serial Monitor
void setSerialMonitorEnabled(bool enabled);  // Function to control Serial Monitor display
bool getSerialMonitorEnabled();              // Function to check Serial Monitor state
bool isCommunicationActive();                // Function to check if PLC communication is working
uint8_t getCurrentSlaveID();                 // Function to get current slave ID

#endif // FATEK_RS232_H
