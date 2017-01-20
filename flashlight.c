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

#define DEFAULT_BOFF  122 // ~3.0V
#define DEFAULT_BWARN 140 // ~3.5V

#define EEPROM_MODE_ADDR  ((uint8_t*)12)
#define EEPROM_BOFF_ADDR  ((uint8_t*)13)
#define EEPROM_BWARN_ADDR ((uint8_t*)14)

static uint8_t leds_mode = 0;

/*
#define UART_BAUD    115200                 // do not use too low rates
#define UART_DELAY   (F_CPU/3/UART_BAUD-1)  // the result delay must be [1..255]
#define UART_TX_DDR  DDRB
#define UART_TX_PORT PORTB
#define UART_TX_PIN  PB3

void uart_init() {
    UART_TX_DDR  |= (1 << UART_TX_PIN);
    UART_TX_PORT |= (1 << UART_TX_PIN);
}

void uart_send_byte(uint8_t b) {
    __asm__ volatile(
        "cbi %[port], %[pin]  ; start bit"                     "\n\t" // 2
        "adiw r26, 0          ; dummy to balance start"        "\n\t" // 2
        "                     ;   and other bits"              "\n\t"
        "in r18, %[port]      ; save whole port to r18"        "\n\t" // 1
        "ldi r19, 0x01        ; pair r19:%[b] is used"         "\n\t" // 1
        "                     ;   as 16 bit register,"         "\n\t"
        "                     ;   preload with stop bit"       "\n\t"
        "1:"                                                   "\n\t"
        "mov r26, %[d]        ; delay (%[d]*3) cycles"         "\n\t" // 1
        "2:"                                                   "\n\t"
        "dec r26"                                              "\n\t" // 1
        "brne 2b"                                              "\n\t" // 1/2
        "bst %[b], 0          ; move LSB of 'b' to r18 via T"  "\n\t" // 1
        "bld r18, %[pin]"                                      "\n\t" // 1
        "out %[port], r18"                                     "\n\t" // 1
        "lsr r19"                                              "\n\t" // 1
        "ror %[b]"                                             "\n\t" // 1
        "brne 1b              ; next bit"                      "\n\t" // 1/2
        :
        : [d] "r"(UART_DELAY), [b] "r"(b), [port] "I"(_SFR_IO_ADDR(UART_TX_PORT)), [pin] "I"(UART_TX_PIN)
        : "r26", "r18", "r19"
    );
}

void uart_send_str(const char* s) {
    for(; *s; ++s)
        uart_send_byte(*s);
}
*/

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
    ADMUX |= (1 << REFS0) | (1 << BAT_MUX); // use internal ref
    ADMUX |= (1 << ADLAR);

    // prescaler 128
    ADCSRA |= (1 << ADPS1) | (1 << ADPS0) | (1 << ADEN);
}

uint8_t get_battery_direct() {
    ADCSRA |= (1 << ADSC);
    while(ADCSRA & (1 << ADSC));

    return ADCH;
}

// filtered
uint8_t get_battery() {
    static uint16_t battery_reg = 0;

    ADCSRA |= (1 << ADSC);
    while(ADCSRA & (1 << ADSC));
    uint8_t bat = get_battery_direct();

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

void battery_calibrate() {
    uint8_t cal = get_battery_direct(); // which level to calibrate?
    uint8_t bat;

    // wait for calibration start
    do {
        set_leds(1);
        _delay_ms(20);
        set_leds(0);
        _delay_ms(20);
        bat = get_battery_direct();
    }
    while(bat == 0 || bat == 0xFF);

    // loop to clear the filter
    for(uint8_t i = 0; i < 16; ++i) {
        _delay_ms(10);
        bat = get_battery();
    }

    // save
    if(cal == 0)
        eeprom_write_byte(EEPROM_BOFF_ADDR, bat);
    else
        eeprom_write_byte(EEPROM_BWARN_ADDR, bat);

    // wait for switch off
    while(true) {
        set_leds(1);
        _delay_ms(200);
        set_leds(0);
        _delay_ms(200);
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
    // init
    //uart_init();
    bool offtime = get_offtime();
    leds_init();
    battery_init();

    // check if we should start battery level calibration
    uint8_t cal = get_battery_direct();
    if(cal == 0 || cal == 0xFF)
        battery_calibrate(); // will never return

    uint8_t bat_off_level  = eeprom_read_byte(EEPROM_BOFF_ADDR);
    if(bat_off_level == 0xFF) bat_off_level = DEFAULT_BOFF;

    uint8_t bat_warn_level = eeprom_read_byte(EEPROM_BWARN_ADDR);
    if(bat_warn_level == 0xFF) bat_warn_level = DEFAULT_BWARN;

    // check mode switch
    load_leds_mode();
    if(offtime) { // it was a click
        next_leds_mode();
        save_leds_mode();
    }
    leds_restore();

    uint8_t bat_warn = 0;
    while(1) {
        // check battery state
        uint16_t bat = get_battery();
        if(bat <= bat_off_level) {
            poweroff();
        }
        else if(bat <= bat_warn_level) {
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

        _delay_ms(100);
    }

    return 0;
}

