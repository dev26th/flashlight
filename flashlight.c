// Created by Anatoli Klassen (AKA dev26th) and dedicated to the public domain.
// See file UNLICENSE for details.

#include <stdlib.h>
#include <stdbool.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>
#include <avr/sleep.h>
#include <util/delay.h>

#define OFFTIME PB4
#define LED1    PB0
#define LED2    PB1
#define BAT     PB2
#define BAT_MUX MUX0

#define BAT_OFF  122*4 // 3.0V
#define BAT_WARN 140*4 // 3.5V

#define EEPROM_MODE_ADDR ((uint8_t*)12)

static uint8_t leds_mode = 0;

bool get_offtime() {
    // check off-time and change the capacitor afterwards
    DDRB &= ~(1 << OFFTIME);
    PORTB &= ~(1 << OFFTIME);

    uint8_t offtime = (PINB & (1 << OFFTIME));

    DDRB |= (1 << OFFTIME);
    PORTB |= (1 << OFFTIME);

    return (offtime != 0);
}

void offtime_disable() {
    DDRB &= ~(1 << OFFTIME);
    PORTB &= ~(1 << OFFTIME);
}

void battery_init() {
    // ADC for battery level
    ADMUX |=  (1 << REFS0) | (1 << BAT_MUX); // use internal ref
    ADMUX &= ~(1 << ADLAR);

    // prescaler 128
    ADCSRA |= (1 << ADPS1) | (1 << ADPS0) | (1 << ADEN);
}

// filtered
uint16_t get_battery() {
    static uint32_t battery_reg = 0;

    ADCSRA |= (1 << ADSC);
    while(ADCSRA & (1 << ADSC));
    uint16_t bat = ADC;

    if(battery_reg == 0) battery_reg = (bat << 4);
    battery_reg = battery_reg - (battery_reg >> 4) + bat;

    return (battery_reg >> 4);
}

void leds_init() {
    // pins for the LEDs
    DDRB  |=   (1 << LED1) | (1 << LED2);
    PORTB &= ~((1 << LED1) | (1 << LED2));
}

void next_leds_mode() {
    ++leds_mode;
    if(leds_mode > 3) leds_mode = 1;
}

void load_leds_mode() {
    leds_mode = eeprom_read_byte(EEPROM_MODE_ADDR);
    if(leds_mode < 1 || leds_mode > 3) leds_mode = 1;
}

void save_leds_mode() {
    eeprom_write_byte(EEPROM_MODE_ADDR, leds_mode);
}

void set_leds(uint8_t mode) {
    uint8_t reg = PORTB;
    reg &= ~((1 << LED1) | (1 << LED2));
    switch(mode) {
        case 1: reg |= (1 << LED1); break;
        case 2: reg |= (1 << LED2); break;
        case 3: reg |= (1 << LED1) | (1 << LED2); break;
    }
    PORTB = reg;
}

void leds_off() {
    set_leds(0);
}

void leds_restore() {
    set_leds(leds_mode);
}

void poweroff() {
    leds_off();
    offtime_disable();
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    while(true) {
        sleep_mode();
    }
}

/*
void pwm_led1_init() {
    TCCR0B |= (1 << CS01) | (1 << CS00);
    TCCR0A |= (1 << WGM01) | (1 << WGM00);
    TCCR0A |= (1 << COM0B1);
    //OCR0B = v;
}
*/

int main(void) {
    bool offtime = get_offtime();
    leds_init();
    battery_init();


    uint8_t delayed_save = 0;
    load_leds_mode();
    if(offtime) { // it was a click
        next_leds_mode();
        delayed_save = 10; // save the new mode 1 sec later
    }
    leds_restore();

    uint8_t bat_warn = 0;
    while(1) {
        // check battery state
        uint16_t bat = get_battery();
        if(bat <= BAT_OFF) {
            poweroff();
        }
        else if(bat <= BAT_WARN) {
            if(bat_warn == 255)
                bat_warn = 254; // begin warn cycle
        }
        else {
            bat_warn = 255;
        }

        if(bat_warn != 255) --bat_warn;

        if(bat_warn < 5)
            leds_off();
        else
            leds_restore();

        // save settings
        if(delayed_save > 0) {
            --delayed_save;
            if(delayed_save == 0) {
                save_leds_mode();
            }
        }

        _delay_ms(100);
    }

    return 0;
}

