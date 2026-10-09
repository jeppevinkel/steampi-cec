//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <libcec/cectypes.h>

// Reads a 2-byte physical address from CEC parameters.
inline uint16_t readPhysicalAddress(const CEC::cec_datapacket& parameters,
                                    const uint8_t byteOffset = 0) {
    if (parameters.size < byteOffset + 2) {
        return static_cast<uint16_t>(CEC_INVALID_PHYSICAL_ADDRESS);
    }
    return static_cast<uint16_t>(parameters[byteOffset] << 8 | parameters[byteOffset + 1]);
}

// 0x1000 -> "1.0.0.0"
inline std::string formatPhysicalAddress(const uint16_t physicalAddress) {
    return std::format("{:x}.{:x}.{:x}.{:x}",
                       (physicalAddress >> 12) & 0xF, (physicalAddress >> 8) & 0xF,
                       (physicalAddress >> 4) & 0xF, physicalAddress & 0xF);
}