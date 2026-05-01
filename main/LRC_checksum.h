/*
 * File: LRC_checksum.h
 * Description: Declaration for LRC checksum utility functions.
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

#ifndef LRC_CHECKSUM_H
#define LRC_CHECKSUM_H

#include <Arduino.h>

/**
 * Calculate LRC (Longitudinal Redundancy Check) checksum
 * Converted from Python LRC_checksum_calculator.py
 * 
 * @param data The data string to calculate LRC for (without STX/ETX)
 * @return String containing the LRC checksum in hex format (2 characters)
 */
String LRC_calc(String data);

#endif
