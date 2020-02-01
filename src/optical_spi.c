/*
Optical SPI

This handles the process of communicating with the microcontroller on the
PAY-Optical board. Using SPI, this microcontroller (PAY-SSM) functions as the master while
the PAY-Optical microcontroller functions as the slave.

Protocol:

PAY sends 2 bytes:
byte 1 = command opcode
byte 2 = well info (or other additional info)

PAY responds by interpreting the command opcode. OPTICAL performs the command.

Sometimes, PAY's requested command always requires data to be returned (usually 24-bit data from the ADC).
In this case, OPTICAL loads 1 byte of data into the SPI Data Register (SPDR).
It will then set its DATA_RDYn line LOW, signaling PAY to initiate a SPI transfer.
The data between PAY's and OPTICAL's SPDR registers are swapped. This is repeated as many times
as necessary until all the data is sent from OPTICAL to PAY.

The SPI Transmission Flag (SPIF) is set at the end of every SPI transmission. It is
automatically cleared when the SPDR register is accessed.

well_info interpreted as:
bit 5 - optical density = 0, fluorescent = 1
bit 4-0 = well number (0-31)
*/

#include "optical_spi.h"

// tracking if SPI in progress
bool spi_in_progress = false;
uint8_t current_well_info = 0;

bool print_spi_transfers = true;


// set up slave select, reset, and DATA_RDYn pins
void init_opt_spi(void) {
    // Initialize PAY-Optical CS pin
    init_cs(OPT_CS, &OPT_CS_DDR );
    set_cs_high(OPT_CS, &OPT_CS_PORT);

    // Initialize PAY-Optical reset pin
    init_cs(OPT_RST, &OPT_RST_DDR);
    set_cs_high(OPT_RST, &OPT_RST_PORT);

    // Initialize PAY-Optical DATA_RDY pin
    OPT_DATA_DDR &= ~_BV(OPT_DATA);     // write 0, set as input pin
    OPT_DATA_PORT |= _BV(OPT_DATA);     // activate input pullup resistor
}


// Resets the PAY-Optical microcontroller
void rst_opt_spi(void) {
    // TODO - check how many cycles is necessary
    set_cs_low(OPT_RST, &OPT_RST_PORT);
    _delay_ms(1000);
    set_cs_high(OPT_RST, &OPT_RST_PORT);
}


// returns status of DATA_RDY pin
uint8_t get_data_pin(void){
    return (OPT_DATA_PIN >> OPT_DATA) & 0x1;
}


// sends 2 bytes to request reading
// byte 1 = cmd_byte
// byte 2 = well_into
void send_opt_spi_cmd(uint8_t cmd_opcode, uint8_t well_info) {
    if (print_spi_transfers) {
        print("SPI TX: %.2x:%.2x\n", cmd_opcode, well_info);
    }

    // Send the command to PAY-Optical to start reading data    
    set_cs_low(OPT_CS, &OPT_CS_PORT);
    send_spi(cmd_opcode);
    set_cs_high(OPT_CS, &OPT_CS_PORT);

    // wait for DATA_RDYn to go LOW, signaling optical is done reading byte 1
    // --> aka loop until DATA_RDYn is no longer HIGH
    uint16_t timeout = UINT16_MAX;
    while (get_data_pin() == 1 && timeout>0){
        timeout--;
    }
    if (timeout == 0) {
        print("TIMEOUT in send cmd\n");
    }

    // print("Sending optical the well data %X\n", well_info);
    // send PAY-OPTICAL the 2nd byte, containing details about the well and type of reading to take
    set_cs_low(OPT_CS, &OPT_CS_PORT);
    send_spi(well_info);
    set_cs_high(OPT_CS, &OPT_CS_PORT);

    // set SPI status to "waiting for OPTICAL to respond"
    spi_in_progress = true;
    current_well_info = well_info;
}


// function called by PAY-SSM in its main loop 'every once in a while' to check up on OPTICAL after it sent it a command
// if PAY-OPTICAL responded properly, proceed with multiple byte SPI transmission
// otherwise, do nothing
void check_received_opt_data(uint8_t num_expected_bytes){
    // if currently waiting for OPTICAL response
    if (spi_in_progress){
        // OPTICAL responded by pulling it's DATA_RDYn line low
        if (get_data_pin()==0){
            print("Found DATA_RDYn low\n");

            uint32_t opt_spi_data = 0;     // Data received from OPTICAL, always right-aligned

            // exchange all the required bytes
            for (uint8_t i = 0; i < num_expected_bytes; i++) {
                set_cs_low(OPT_CS, &OPT_CS_PORT);
                uint8_t new_opt_data = send_spi(0x00);
                set_cs_high(OPT_CS, &OPT_CS_PORT);

                // OPTICAL ready to send data
                opt_spi_data = (opt_spi_data<<8) | new_opt_data;

                // small delay to give time for DATA_RDYn to go high
                _delay_ms(100);
            }

            // successfully sent command, and received all bytes from OPTICAL
            spi_in_progress = false;

            // creates a CAN message, but we'll use print statements for now
            print("recieved data from well_info 0x%X: 0x%X\n", current_well_info, opt_spi_data);
        
            // create CAN message
            uint8_t tx_msg[8] = { 0x00 };
            tx_msg[0] = CAN_PAY_OPT;            // opcode
            tx_msg[1] = current_well_info;      // field number
            tx_msg[2] = CAN_STATUS_OK;          // status, 0x00 = ok
            tx_msg[3] = 0x00;                   // unused
            tx_msg[4] = 0x00;                   // data bits 4-7 read as uint32_t
            tx_msg[5] = (opt_spi_data >> 16) & 0xFF; 
            tx_msg[6] = (opt_spi_data >> 8) & 0xFF;
            tx_msg[7] = (opt_spi_data) & 0xFF;
            // Enqueue TX message to transmit
            enqueue(&tx_msg_queue, tx_msg);
        }
    }

    // no response from OPTICAL -> do nothing
}
