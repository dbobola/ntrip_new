#include <iostream>
#include <string>
#include "ntrip/ntrip_util.h"

int main() {
    std::cout << "Testing NTRIP Auto-Selection Functions" << std::endl;
    
    // Test distance calculation
    double lat1 = 37.7749, lon1 = -122.4194;  // San Francisco
    double lat2 = 37.7849, lon2 = -122.4094;  // 1km north
    double lat3 = 37.7649, lon3 = -122.4294;  // 1km south
    
    double dist1 = libntrip::CalculateDistance(lat1, lon1, lat2, lon2);
    double dist2 = libntrip::CalculateDistance(lat1, lon1, lat3, lon3);
    
    std::cout << "Distance SF to North: " << dist1 << " meters" << std::endl;
    std::cout << "Distance SF to South: " << dist2 << " meters" << std::endl;
    
    // Test position parsing from header
    double parsed_lat, parsed_lon;
    std::string header1 = "lat=37.7749,lon=-122.4194";
    std::string header2 = "37.7849,-122.4094";
    
    if (libntrip::ParsePositionFromHeader(header1, &parsed_lat, &parsed_lon) == 0) {
        std::cout << "Parsed header1: lat=" << parsed_lat << ", lon=" << parsed_lon << std::endl;
    }
    
    if (libntrip::ParsePositionFromHeader(header2, &parsed_lat, &parsed_lon) == 0) {
        std::cout << "Parsed header2: lat=" << parsed_lat << ", lon=" << parsed_lon << std::endl;
    }
    
    // Test GGA parsing
    std::string gga = "$GPGGA,123456.00,3746.4940000,N,12225.1640000,W,1,12,1.2,123.4,M,-2.860,M,,0000*7A";
    if (libntrip::ParsePositionFromGGA(gga, &parsed_lat, &parsed_lon) == 0) {
        std::cout << "Parsed GGA: lat=" << parsed_lat << ", lon=" << parsed_lon << std::endl;
    }
    
    std::cout << "All tests completed!" << std::endl;
    return 0;
} 