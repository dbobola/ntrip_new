# NTRIP Caster Auto-Selection Feature

This document explains how to use the new auto-selection feature that automatically selects the closest base station based on client position.

## Overview

The auto-selection feature allows clients to connect with mountpoint "auto" and automatically receive correction data from the nearest base station based on their GPS coordinates.

## Base Station Registration

Base stations must provide their position during registration. This can be done in two ways:

### Method 1: Custom Position Header
```
POST /BASE1 HTTP/1.1
Authorization: Basic dXNlcjE6cGFzczE=
Position: lat=37.7749,lon=-122.4194
Ntrip-STR: STR;BASE1;BASE1;Carrier;Nav;Freq;Auth;Fee;Bitrate;
User-Agent: BaseStation/1.0
Content-Length: 0

```

### Method 2: Position in Ntrip-STR (misc field)
```
POST /BASE2 HTTP/1.1
Authorization: Basic dXNlcjI6cGFzczI=
Ntrip-STR: STR;BASE2;BASE2;Carrier;Nav;Freq;Auth;Fee;Bitrate;37.7849,-122.4094
User-Agent: BaseStation/1.0
Content-Length: 0

```

## Client Connection

### Auto-Selection Request
To use auto-selection, clients must provide their position and use mountpoint "auto":

```
GET /auto HTTP/1.1
Authorization: Basic dXNlcjE6cGFzczE=
Position: lat=37.7849,lon=-122.4194
User-Agent: Client/1.0

```

### Standard Mountpoint Request
For standard connection to a specific mountpoint:

```
GET /BASE1 HTTP/1.1
Authorization: Basic dXNlcjE6cGFzczE=
User-Agent: Client/1.0

```

## How It Works

1. **Base Station Registration**: When a base station connects, the caster stores its position
2. **Client Connection**: When a client connects with "auto" mountpoint:
   - Client must provide position via Position header
   - Caster calculates distance to all available base stations
   - Caster selects the closest base station with valid authentication
   - Client receives correction data from the selected base station

## Distance Calculation

The system uses the Haversine formula to calculate the great-circle distance between two points on Earth's surface:

```
Distance = R * arccos(sin(lat1) * sin(lat2) + cos(lat1) * cos(lat2) * cos(lon2 - lon1))
```

Where R = 6,371,000 meters (Earth's radius)

## Example Scenario

### Base Stations
- BASE1: lat=37.7749, lon=-122.4194 (San Francisco)
- BASE2: lat=37.7849, lon=-122.4094 (San Francisco, 1km north)
- BASE3: lat=37.7649, lon=-122.4294 (San Francisco, 1km south)

### Client
- Position: lat=37.7799, lon=-122.4144 (between BASE1 and BASE2)

### Result
- Distance to BASE1: ~550 meters
- Distance to BASE2: ~650 meters  
- Distance to BASE3: ~1,100 meters
- **Auto-selection**: BASE1 (closest)

## Error Handling

- **No client position**: Returns 400 Bad Request
- **No base stations with position**: Returns 503 Service Unavailable
- **Authentication failure**: Returns 401 Unauthorized
- **Invalid position format**: Returns 400 Bad Request

## Logging

The caster provides detailed logging:
```
Base station position: lat=37.7749, lon=-122.4194
Base station registered: BASE1 (has_position=yes)
Client position from header: lat=37.7799, lon=-122.4144
Distance to BASE1: 550.23 meters
Distance to BASE2: 650.45 meters
Auto-selected mountpoint: BASE1 (distance: 550.23 meters)
```

## Benefits

1. **Optimal Performance**: Clients automatically get the best correction data
2. **Simplified Configuration**: No need to manually select base stations
3. **Network Efficiency**: Reduces baseline length for better RTK performance
4. **Scalability**: Works with any number of base stations
5. **Fallback**: Standard mountpoint selection still works for specific needs 