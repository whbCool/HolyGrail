# TEA@CPP Escape Room 2026: The Holy Grail

<img width="7400" height="3061" alt="Holy Grail2" src="https://github.com/user-attachments/assets/53cdbbcb-ab72-403f-9037-f6201b524167" />

 
The _Holy Grail_ is a custom PCB, designed for the Indiana Jones puzzle within TEA@CPP's "Retro Rewind" 2026 Escape Room. 

This board runs on an ESP32-WROOM-32UE and uses a CD74HC138QM96Q1 to select between different UART channels.

The CD74HC138QM96Q1 addresses one of six SC16IS752IPW_128 chips. Each SC16IS752IPW_128 chip has two UART channels.

Each SC16IS752IPW_128 chip is connected to two LD2410C sensors, for a total of 12 sensors.

The ESP will address these SC16IS752IPW_128 chips using the CD74HC138QM96Q1, and the SC16IS752IPW_128 chips will communicate with the ESP using SPI

The ESP will take the incoming data from the SC16IS752IPW_128 chips, format it into a usable JSON payload, and send it to an MQTT broker.

Major Components:
- ESP32-WROOM-32UE
- SC16IS752IPW_128
- CD74HC138QM96Q1
- SN74LVC1G125DBVT
- CDCLVC1106PWR
- 830072403
- LM3940IMP-3.3_NOPB
- AP63205WU-7
- 5747842-2
- W5500
- USBLC6-2SC6
- HR911105A
- MMBT3904
- CP2102N-A02-GQFN24
- MF-MSMF250_16X-2
- LM393DR

Design Requirements:
1. The board will read data from 12 LD2410C sensors using SPI-to-UART converters.
2. The board will aggregate sensor data and package it into a JSON payload to be sent to an MQTT broker.
3. The board will be powered from a 12V DC supply.
4. The board has a USB-C port for programming and debugging.

Link to Schematics: https://drive.google.com/drive/folders/14Y64IZpTN127Q0mva5SdtTZ1QgllTaBJ
