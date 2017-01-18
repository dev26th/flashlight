flashlight.hex: flashlight.elf
	avr-objcopy -O ihex flashlight.elf flashlight.hex

flashlight.elf: flashlight.c
	avr-gcc -std=c99 -DF_CPU=9600000 -mmcu=attiny13 -Os -o flashlight.elf flashlight.c
	avr-size flashlight.elf

program: flashlight.hex
	avrdude -p t13 -c usbasp -U flash:w:flashlight.hex

clean:
	rm flashlight.elf flashlight.hex

.PHONY: hex elf clean

