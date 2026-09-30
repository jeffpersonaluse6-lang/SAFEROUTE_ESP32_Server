# SAFEROUTE ESP32 Local Emergency Server

This Arduino sketch turns an ESP32-WROOM-32 into a small local
emergency-report server. Phones connect to
`SAFEROUTE-NET`; public internet is not required.

## Arduino IDE settings

- Board package: **esp32 by Espressif Systems**
- Board: **ESP32 Dev Module**
- Upload speed: **921600** (use 115200 if the upload is unreliable)
- Flash frequency: **80 MHz**
- Flash mode: **QIO**
- Partition scheme: **Default 4MB with spiffs**
- Serial Monitor: **115200 baud**
- Extra libraries: **none**

Open `SAFEROUTE_ESP32_Server.ino`, change `AP_PASSWORD` and
`STAFF_API_KEY`, then upload it. Connect a phone or laptop to
`SAFEROUTE-NET`. The default server address is `http://192.168.4.1`.

## Test from Windows PowerShell

Replace the key below with the value in the sketch.

```powershell
$headers = @{ 'X-SAFEROUTE-KEY' = 'CHANGE-THIS-STAFF-KEY' }

Invoke-RestMethod -Method Post `
  -Uri 'http://192.168.4.1/report' `
  -Headers $headers `
  -ContentType 'application/json' `
  -Body '{"hazard_id":"fire_1","type":"fire","zone":"CAMPUS","x":1510.0,"y":620.0,"radius":40.0,"building_id":"","floor":1,"active":true}'

Invoke-RestMethod -Method Get -Uri 'http://192.168.4.1/status'

Invoke-RestMethod -Method Post `
  -Uri 'http://192.168.4.1/clear' `
  -Headers $headers `
  -ContentType 'application/json' `
  -Body '{"hazard_id":"fire_1"}'
```

Accepted report types are `fire`, `blocked_path`, and `active_threat`.
The server keeps up to 16 active reports in RAM. Posting a new `hazard_id`
adds a report; posting that same ID updates it; clearing an ID removes only
that report. `GET /status`, `/report`, and `/clear` return the complete current
snapshot in `hazards: []`, along with a snapshot `revision`. Each hazard has a
stable `hazard_id`, exact position/radius/building/floor or a compatible zone,
and a `moving` flag for active-threat movement updates. Restarting the ESP32
clears the in-memory reports.

## SAFEROUTE app behavior

The normal app polls `GET /status` several times per second. Staff place and
select hazards through the existing map controls, press **Report Hazard**, and
enter the staff password. For a reported moving threat, the app keeps the
password in memory only while that report is active so it can publish position
updates using the same hazard ID. Clearing it removes that temporary
credential. **Clear Report** uses the same isolated staff-authenticated write
path. Normal users never receive the staff password.

The app accepts a building label, campus-object label/type, or saved object ID
as a zone. Add `_F1`, `_F2`, and so on to target a building floor. Useful
aliases include:

- `BUILDING_A_F1` through `BUILDING_A_F4` for `SHS-ACADEMIC`
- `JHS_A_F1`, `JHS_B_F2`, `JHS_C_F1`, and `JHS_D_F1`
- `SHS_TVL_A_F1`, `SHS_TVL_B_F3`, and `SHS_TVL_C_F2`
- `MALACANANG_F1` and `MALACANANG_F2`
- `CANTEEN`, `GAZEBO`, `MAIN_COURT`, `JHS_FACULTY`, and `CLINIC`

An unknown zone is rejected by the app bridge and does not create a hazard.
The server URL defaults to `http://192.168.4.1`. For a test server, override it
when building Flutter with:

```text
--dart-define=SAFEROUTE_SERVER_URL=http://192.168.4.1
```
