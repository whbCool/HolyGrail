TEA@CPP Escape Room 2026: The Holy Grail

The Holy Grail is a custom PCB that is designed to be used as a part of an escape room puzzle. 

This board runs on an ESP32 and uses a CD74HC138QM96Q1 to decode address lines from the ESP32 to select between different UART channels.

The CD74HC138QM96Q1 addresses six SC16IS752IPW_128 chips. Each SC16IS752IPW_128 chip has two UART channels.

Each SC16IS752IPW_128 chip is connected to two LD2410C sensors, for a total of 12 sensors.

The ESP will adress these SC16IS752IPW_128 chips using the CD74HC138QM96Q1, and the SC16IS752IPW_128 chips will communicate with the ESP using SPI

The ESP will take the incoming data from the SC16IS752IPW_128 chips, format it into a usable JSON payload, and send it to an MQTT broker.

Major Components:
ESP32-WROOM-32UE
SC16IS752IPW_128
CD74HC138QM96Q1
SN74LVC1G125DBVT
CDCLVC1106PWR
830072403
LM3940IMP-3.3_NOPB
AP63205WU-7
5747842-2
W5500
USBLC6-2SC6
HR911105A
MMBT3904
CP2102N-A02-GQFN24
MF-MSMF250_16X-2
LM393DR

Design Requirements:
1. The board will read data from 12 LD2410C sensors using an spi to uart converter.
2. The board will aggregate sensor data and package it into a format that can be sent to an MQTT broker.
3. The board will be powered from a 12V DC supply.
4. The board has a USB-C port for programming and debugging.
5. The board has a reset button and a boot button.

Link to Schematics: https://drive.google.com/drive/folders/14Y64IZpTN127Q0mva5SdtTZ1QgllTaBJ
