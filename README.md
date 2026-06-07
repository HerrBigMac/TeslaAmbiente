# Tesla Ambiente ESP32 Project

ESP32-C6/C3 Tesla ambient lighting project using ESP-NOW, CAN bus data, web controls and OTA. Built to evolve toward replacing the S3XY Commander. Still in development.

## Overview

This project is a custom Tesla ambient lighting system based on ESP32 Supermini boards. It reads vehicle and chassis CAN bus data, controls LED strips in the dashboard and doors, and synchronizes modules wirelessly with ESP-NOW.

The system is still in active development. The long-term goal is to replace more functions normally handled by external commander-style modules while keeping the setup modular, configurable, and updateable over the air.

## Main Features

- Dashboard ambient LED control
- Door ambient LED control
- ESP-NOW communication between ESP modules
- CAN bus reading for vehicle state
- CAN bus reading for chassis / driver-assistance signals
- Web overlay for configuration
- OTA firmware update support
- Adjustable base color, brightness, effects, and presets
- Automatic brightness based on vehicle display brightness
- Sleep / wake behavior based on mirror and vehicle state
- Welcome and goodbye dashboard animations
- Charging state visualization on the dashboard
- Automatic charging animation timeout
- Autopilot dashboard indication
- Blind spot warning support
- Side collision warning support while reversing
- Door-open highlight support

## Hardware

The project is designed around small ESP32 boards:

- ESP32-C6 Supermini / Tiny Mini
- ESP32-C3 Supermini for door slaves
- Normal ESP32 Dev board for the master
- SN65HVD230 CAN transceivers
- WS2812 / WS2812B compatible RGB LED strips
- Suitable 5 V LED power supply or buck converter
- Common ground between ESP, LED power supply, and vehicle reference ground

The current master is a normal ESP32 Dev board. It is used as the web/control hub so the dashboard ESP32-C6 does not also have to handle all master-side web, command, and coordination work.

Power conversion used in this build:

- Each door uses a 12 V to 5 V 3 A converter for the LED/ESP setup.
- The dashboard uses one 12 V to 3.3 V 2 A converter for the ESP32-C6.
- The dashboard LEDs use a separate 12 V to 5 V 5 A converter.

The LED strips used in this build are simple AliExpress ambient strips that normally come with a USB control dongle. In my wiring, red was minus and black was plus, so check polarity carefully before connecting power. Do not trust wire colors blindly.

LED strip link:

[AliExpress ambient LED strip](https://de.aliexpress.com/item/1005008083921156.html?spm=a2g0o.order_list.order_list_main.16.21ef5c5fijuwUP&gatewayAdapt=glo2deu)

## Current Pin Layout

Standalone dashboard ESP32-C6:

```text
LED data:       GPIO4
Vehicle CAN TX: GPIO1
Vehicle CAN RX: GPIO2
Chassis CAN TX: GPIO15
Chassis CAN RX: GPIO14
```

Door slave:

```text
LED data: GPIO21
```

## Module Layout

### Tesla_Dash_Standalone_C6

Standalone dashboard controller. One ESP32-C6 reads both CAN buses and directly drives the dashboard LED strip. It also provides the web overlay and OTA update page.

This standalone firmware is included for builds where you do not want to buy or use the S3XY Strip. The ESP32-C6 handles CAN reading, dashboard LED output, the web overlay, and OTA by itself.

Important functions:

- VehicleBus and ChassisBus reading
- Dashboard LED control
- Charging animation
- Autopilot color
- Blind spot and side collision warning
- Door-open dashboard highlight
- Web configuration
- OTA update

### Tesla_Dash_CANBridge_C6_OTA

Combined CAN bridge and dashboard controller. This ESP32-C6 reads VehicleBus and ChassisBus data, sends data to the master via ESP-NOW, and can control the dashboard strip directly.

### Tesla_Ambiente_Master

Master controller with web overlay. It receives CAN data packets via ESP-NOW and forwards LED commands/settings to door slaves and dashboard modules.

Important functions:

- Web configuration
- Presets
- Target selection for all zones, single doors, or dashboard
- ESP-NOW command dispatch
- OTA trigger commands for slaves / bridge modules
- Door ACK retry handling

### Tesla_Ambiente_Door_Slave_OTA

Door LED slave firmware. This receives ESP-NOW LED commands, controls the configured LED range, supports OTA mode, and can acknowledge received commands.

Before flashing a door slave, the device letter must be adjusted in the sketch so the module knows which door it belongs to. Flash the same firmware to each door, but change the target letter first.

Door target mapping:

```text
F = front right
G = front left
R = rear left
L = rear right
```

### Tesla_VehicleBus_Reader_Charge

VehicleBus reader variant with display brightness, mirror state, door state, charging state, and battery SOC decoding.

## CAN Signals

The project currently uses signals such as:

- Display brightness
- Battery state of charge
- Charging state
- Mirror fold state
- Door latch state
- Trunk / liftgate state
- Autopilot state
- Blind spot warning
- Side collision warning

The exact signal mapping is based on the Tesla Model 3 / Model Y DBC data used during development.

## Web Overlay

The web overlay allows configuration without reflashing:

- Base color
- Manual brightness
- Automatic CAN brightness
- Effects
- Presets
- Dashboard LED count
- Blind spot enable / disable
- Blind spot percentage
- Door-open highlight enable / disable
- Autopilot color
- Side collision warning enable / disable
- Charging animation timeout
- OTA update access

For the standalone dashboard firmware, the charging animation timeout can be changed in minutes. A value of `0` enables the short test timeout.

## OTA Updates

OTA support is included so installed modules can be updated without removing panels again.

Typical OTA behavior:

- Master OTA can stay available from the web overlay.
- Door slaves can open a temporary access point for OTA.
- Dashboard / CAN bridge modules can open a temporary OTA access point when commanded.

Firmware uploads use `.bin` files generated by the Arduino build process.

## LED Behavior Priorities

The dashboard prioritizes high-importance states over normal ambient lighting. The intended priority is:

1. Charging animation
2. Welcome / goodbye animation
3. Door-open indication
4. Autopilot indication
5. Side collision warning
6. Blind spot warning
7. Optional blinker indication
8. Normal ambient lighting

Charging animation can time out and keep the dashboard off while charging continues.

## Development Status

This project is still in development. Some features depend on vehicle firmware, CAN signal availability, wiring quality, ESP-NOW reliability, and the exact Tesla model/configuration.

Current goals:

- Improve reliability of ESP-NOW communication
- Keep OTA update paths available
- Refine dashboard warning priorities
- Improve web overlay usability
- Continue validating CAN signals in real vehicle conditions

## Safety Notice

This is a custom hobby project connected to vehicle CAN buses and lighting hardware. Use listen-only CAN mode for readers, avoid writing to vehicle CAN, fuse LED power properly, and test carefully. Incorrect wiring or unsafe power handling can damage electronics or create fire risk.

Do not rely on this project as a safety system. Vehicle warnings from the original Tesla UI remain the primary source of driver information.
