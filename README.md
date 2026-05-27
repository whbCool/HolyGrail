# TEA@CPP Escape Room 2026: The Holy Grail

<img width="7400" height="3061" alt="update_noTB" src="https://github.com/user-attachments/assets/767ab534-400e-4e8f-8da8-90152007331e" />

The _Holy Grail_ is a custom PCB, designed for the Indiana Jones puzzle as a part of TEA@CPP's "Retro Rewind" 2026 Escape Room. 

This repository includes the KiCad schematic, as well as the code database for the board.

For more details on the code, check out [the introductory walkthrough here](walkthrough.md).

Major Components:
- ESP32-WROOM-32UE
- SC16IS752IPW_128
- CD74HC138QM96Q1
- SN74LVC1G125DBVT
- CDCLVC1106PWR
- 7447715003
- LMR33640
- AP63205WU-7
- 5747842-2
- W5500
- USBLC6-2SC6
- TMJ0026ABNL
- MMBT3904
- CP2102N-A02-GQFN24
- 1812L150_24MR
- LM393DR

Design Requirements:
1. Read data from 12 LD2410C sensors using SPI-to-UART converters.
2. Aggregate sensor data and package it into a JSON payload to be sent to an MQTT broker.
3. USB port for programming and debugging.

More Info: https://drive.google.com/drive/folders/14Y64IZpTN127Q0mva5SdtTZ1QgllTaBJ
