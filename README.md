# flashlight

A firmware for flashlight driver based on ATtiny13A.

Public domain - see file UNLICENSE.

##Hardware

Designed for power button (not tact one), one Li-Ion cell. 

(a schematic will be added later)

- PB0 - one AMC7135
- PB1- two AMC7135's
- PB3 - divider 19k1 and 4k7 between VCC and GND (for battery monitoring)
- PB4 - resistor 1M and then ceramic capacitor 1u to GND (for off-time measurement)

*Note:* well-known AK-47A driver could be simply modified for this schematic.

##Handling

Tree modes: 0.35A, 0.7A, 1.05A. No PWM. Quick off-on to switch to next mode. Remembers last selected mode.

Will go periodically off for half a second if battery is low (< 3.5V) and power off in battery is empty (< 3.0V). But exact voltage may very for different chips.

##Programming

Fuses (avrdude format):

>-U lfuse:w:0x7a:m -U hfuse:w:0xff:m
