// Test script to verify RS485 command generation
// Compile with: g++ -o test_rs485 test_rs485_commands.cpp -I. -lesp_crc

#include <iostream>
#include <iomanip>
#include <string>
#include "includes/set_include.h"

int main() {
    std::cout << "Testing RS485 command generation...\n\n";
    
    // Test command generation for battery 0
    int addr = 2;  // Pylontech address
    int cid2 = 0x42;  // Analog data request
    int batt_num = 0;
    
    std::string cmd = rs485_make_cmd(addr, cid2, batt_num);
    
    std::cout << "Generated command: " << cmd << "\n";
    std::cout << "Command length: " << cmd.length() << " characters\n\n";
    
    // Break down the command
    if (cmd.length() >= 18) {
        std::string frame = cmd.substr(1, cmd.length() - 6);  // Exclude ~ and checksum
        std::string checksum = cmd.substr(cmd.length() - 5, 4);
        
        std::cout << "Frame (without ~ and checksum): " << frame << "\n";
        std::cout << "Frame length: " << frame.length() << " characters\n";
        std::cout << "Checksum: " << checksum << "\n\n";
        
        // Verify the checksum
        std::string calculated_chk = rs485_calc_chksum(frame);
        std::cout << "Calculated checksum: " << calculated_chk << "\n";
        std::cout << "Checksum match: " << (checksum == calculated_chk ? "✅ YES" : "❌ NO") << "\n\n";
        
        // Break down frame components
        if (frame.length() >= 12) {
            std::string header = frame.substr(0, 2);
            std::string addr_str = frame.substr(2, 2);
            std::string cid2_str = frame.substr(4, 2);
            std::string lenid = frame.substr(6, 4);
            std::string info = frame.substr(10, 2);
            
            std::cout << "Frame breakdown:\n";
            std::cout << "  Header: " << header << " (should be 20)\n";
            std::cout << "  Address: " << addr_str << " (should be " << std::hex << addr << ")\n";
            std::cout << "  CID2: " << cid2_str << " (should be " << std::hex << cid2 << ")\n";
            std::cout << "  LENID: " << lenid << "\n";
            std::cout << "  Info: " << info << " (should be " << std::hex << batt_num << ")\n";
            
            // Analyze LENID
            std::string lchksum_str = lenid.substr(0, 1);
            std::string length_str = lenid.substr(1, 3);
            
            std::cout << "\nLENID breakdown:\n";
            std::cout << "  Length checksum: " << lchksum_str << "\n";
            std::cout << "  Length: " << length_str << " (should be 002 for 1 byte info)\n";
            
            // Calculate what LENID checksum should be
            int info_hex_len = 2;  // 1 byte = 2 hex chars
            int len_digit_sum = (info_hex_len / 256) + ((info_hex_len / 16) % 16) + (info_hex_len % 16);
            int expected_lchksum = (~len_digit_sum + 1) & 0xF;
            
            std::cout << "  Expected LENID checksum: " << std::hex << expected_lchksum << "\n";
            std::cout << "  Actual LENID checksum: " << lchksum_str << "\n";
            std::cout << "  LENID checksum match: " << (std::stoi(lchksum_str, nullptr, 16) == expected_lchksum ? "✅ YES" : "❌ NO") << "\n";
        }
    }
    
    return 0;
}