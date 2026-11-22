# Bill of materials

Machine-readable version: [`BOM.csv`](BOM.csv). Verify every supplier part number on the supplier's site before ordering.

## Carrier board (soldered)

| Ref | Qty | Part | MPN | Digi-Key | Fitted |
|---|---|---|---|---|---|
| PCB | 1 | Carrier PCB, 60 x 42 mm, 2 layer (JLCPCB, gerbers in `../pcb/fab/`) | | | yes |
| J1 | 1 | JST PH 2-pin 2.0 mm header, vertical (LiPo connector) | B2B-PH-K-S | 455-1704-ND | yes |
| J2, J3 | 2 | 1x9 female socket, 2.54 mm, 8.5 mm tall | SSK02-FPH-254-A9 | 2223-SSK02-FPH-254-A9-ND | yes |
| U1 sockets | 2 | 1x7 female socket, 2.54 mm, 8.5 mm tall | SSK02-FPH-254-A7 | 2223-SSK02-FPH-254-A7-ND | yes |
| SW1 | 1 | Tactile switch, 6 mm, B3F-1000 style | B3F-1000 | SW400-ND | yes |
| SW2 | 1 | Slide switch SPDT, E-Switch EG1218 | EG1218 | EG1903-ND | yes |
| R1 | 1 | 0 ohm, 0603 | RC0603JR-070RL | 311-0.0GRCT-ND | optional |
| TP1, TP2 | 2 | Test pads (on the PCB) | | | yes |

Tip: buy 3 of the JST header (spares and a test-fit), and a spare 0603 resistor.

## Modules and other parts

| Qty | Part | Notes |
|---|---|---|
| 1 | Seeed Studio XIAO ESP32-S3 (not Sense), 2 x 7 male pins | on-board charger, USB-C |
| 1 | Adafruit EYE SPI breakout 5613, 2 x 9 male pins | |
| 1 | Adafruit 1.54" 240x240 IPS TFT with microSD, 3787 | ST7789 |
| 1 | Adafruit 18-pin 0.5 mm FFC cable, 5462 | |
| 1 | LiPo 3.7 V 1000 mAh, 503450, JST PH 2.0 mm plug | check red = + with a meter |
| 1 | microSD card (optional) | game high score |
| 1 | about 40 mm of 28-30 AWG wire | XIAO BAT+ to TP2 |

## 3D printed case

| Qty | File |
|---|---|
| 1 | `case/stl/Case_front.stl` |
| 1 | `case/stl/Case_back.stl` |
| 1 | `case/stl/Case_plate.stl` |
| 1 | `case/stl/Part_button_plunger.stl` |
| 1 | `case/stl/Part_switch_cap.stl` |
| 4 | M2.5 screws for the back shell, one per corner hole |


