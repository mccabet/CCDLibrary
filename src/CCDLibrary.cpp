﻿/*
 * CCDLibrary (https://github.com/laszlodaniel/CCDLibrary)
 * Copyright (C) 2020, László Dániel
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <CCDLibrary.h>
#include <util/atomic.h>
#include <util/delay.h>

CCDLibrary CCD;

CCDLibrary::CCDLibrary()
{
    // Empty.
}

static void isrIdle()
{
    CCD.busIdleInterruptHandler();
}
    
static void isrActiveByte()
{
    CCD.activeByteInterruptHandler();
}

void CCDLibrary::begin(bool interruptsAvailable, uint8_t busIdleBits, bool verifyRxChecksum, bool calculateTxChecksum)
{
    // interruptsAvailable:
    //   INTERRUPTS: enables 1 MHz clock signal on D11/PB5 pin. Useful for the CDP68HC68S1 CCD-bus transceiver IC. 
    //               Bus-idle and arbitration detection is handled by this IC and signaled as external interrupts:
    //                 IDLE_PIN - Arduino pin connected to CDP68HC68S1's IDLE-pin.
    //                 CTRL_PIN - Arduino pin connected to CDP68HC68S1's CTRL-pin.
    //   NO_INTERRUPTS: disables 1 MHz clock signal on D11/PB5 pin. The library handles bus-idle and arbitration detection based on timing and bit-manipulation.
    // busIdleBits:
    //   IDLE_BITS_XX: sets the number of consecutive 1-bits sensed as CCD-bus idle condition (including stop bit of the last message byte).
    //   IDLE_BITS_10: default idle bits is 10 according to the CDP68HC68S1 datasheet. It should be changed if messages are not coming through properly.
    // verifyRxChecksum:
    //   ENABLE_RX_CHECKSUM: verifies received messages against their last checksum byte and ignores them if broken.
    //   DISABLE_RX_CHECKSUM: accepts received messages without verification.
    // calculateTxChecksum:
    //   ENABLE_TX_CHECKSUM: calculates the checksum of outgoing messages and overwrites the last message byte with it.
    //   DISABLE_TX_CHECKSUM: sends messages as they are, no checksum calculation is perfomed.
    
    _interruptsAvailable = interruptsAvailable;
    _busIdleBits = busIdleBits;
    _verifyRxChecksum = verifyRxChecksum;
    _calculateTxChecksum = calculateTxChecksum;
    _messageLength = 0;
    _lastMessageRead = true;
    serialInit(CCD_UBRR);
    
    if (_interruptsAvailable)
    {
        // Enable 1 MHz clock signal for the CDP68HC68S1 transceiver.
        noInterrupts();
        TCCR1A = 0;                            // clear register
        TCCR1B = 0;                            // clear register
        TCNT1 = 0;                             // clear counter
        DDRB |= (1 << DDB5);                   // set OC1A/PB5 as output
        TCCR1A |= (1 << COM1A0);               // toggle OC1A on compare match
        OCR1A = 7;                             // top value for counter, toggle after counting to 8 (0->7) = 2 MHz interrupt ( = 16 MHz clock frequency / 8)
        TCCR1B |= (1 << WGM12) | (1 << CS10);  // CTC mode, prescaler clock/1 (no prescaler)
        
        // Setup external interrupts for bus-idle and active byte detection.
        pinMode(IDLE_PIN, INPUT_PULLUP);
        pinMode(CTRL_PIN, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(IDLE_PIN), isrIdle, FALLING);
        attachInterrupt(digitalPinToInterrupt(CTRL_PIN), isrActiveByte, FALLING);
        interrupts();
        
        _busIdle = true; // start with bus-idle condition
    }
    else
    {
        // Disable 1 MHz clock signal.
        noInterrupts();
        TCCR1A = 0;
        TCCR1B = 0;
        TCNT1  = 0;
        OCR1A  = 0;
        
        // Detach external interrupts if previously assigned.
        detachInterrupt(digitalPinToInterrupt(IDLE_PIN));
        detachInterrupt(digitalPinToInterrupt(CTRL_PIN));
        interrupts();
        
        // Enable bus-idle timer.
        _busIdle = false; // start with bus-busy condition
        busIdleTimerInit();
        busIdleTimerStart();
    }
}

bool CCDLibrary::available()
{
    return !_lastMessageRead;
}

uint8_t CCDLibrary::read(uint8_t *target)
{
    // Copy last message to target buffer.
    for (uint8_t i = 0; i < _messageLength; i++) target[i] = _message[i];
    
    _lastMessageRead = true; // set flag
    return _messageLength;
}

uint8_t CCDLibrary::write(uint8_t *buffer, uint8_t bufferLength)
{
    // Return values:
    //   0: ok
    //   1: zero buffer length
    //   2: timeout
    //   3: data collision
    
    if (bufferLength == 0) return 1;
    
    for (uint8_t i = 0; i < bufferLength; i++) _serialTxBuffer[i] = buffer[i]; // copy message bytes to the transmit buffer
    
    if (_calculateTxChecksum && (bufferLength > 1)) // calculate message checksum if needed, minimum message length is 2 bytes
    {
        uint8_t checksum = 0;
        uint8_t checksumLocation = bufferLength - 1;
        for (uint8_t i = 0; i < checksumLocation ; i++) checksum += buffer[i];
        _serialTxBuffer[checksumLocation] = checksum; // overwrite last byte in the message with the correct checksum value
    }
    
    _serialTxBufferPos = 0; // reset buffer position
    _serialTxLength = bufferLength; // save message length
    
    bool timeout = false;
    uint32_t timeout_start = millis();
    
    while (!_busIdle && !timeout) // wait for bus idle condition or timeout (1 second)
    {
        if (millis() - timeout_start > 1000) timeout = true;
    }
    
    if (timeout) return 2;
    
    // First transmitted byte (ID-byte) is special, it is used to decide bus arbitration when multiple 
    // modules are trying to send a message at the same time. 
    // During transmission every transmitted bit of the first byte needs to be inspected against the 
    // simultaneously received bit in real time and block further bit transmission if data collision occurs. 
    // This can happen if another module is sending an ID-byte value lower 
    // than ours thus overwriting our 1 bits with its 0 bits. 
    // A module sending the lowest ID-byte will win the bus arbitration procedure, and others must block further 
    // data transmission.
    
    _busIdle = false; // clear flag
    
    if (_interruptsAvailable) // CDP68HC68S1 handles arbitration detection internally
    {
        // Enable UDRE interrupt to begin message transmission. That's it.
        // Hopefully the message is being reflected on the CCD-bus, but if not then
        // the CDP68HC68S1 chip disabled its transmitter automatically due to data collision.
        // At that point in time whatever data is thrown at it is just ignored and never reaches the CCD-bus.
        UCSR1B |= (1 << UDRIE1);
        
        return 0;
    }
    else
    {
        // Arduino board's hardware UART does not support bit-level manipulation of the data register 
        // so it is turned off temporarily.
        UCSR1B &= ~(1 << RXCIE1) & ~(1 << RXEN1) & ~(1 << TXEN1) & ~(1 << UDRIE1);
        
        // Setup RX/TX pins manually.
        RX_DDR &= ~(1 << RX_P); // RX is input
        TX_DDR |= (1 << TX_P); // TX is output
        RX_PORT |= (1 << RX_P); // RX internal pullup resistor enabled
        TX_PORT |= (1 << TX_P); // TX idling at logic high
        
        // Prepare variables.
        uint8_t IDbyteTX = _serialTxBuffer[0];
        uint8_t IDbyteRX = 0;
        bool currentRxBit = false;
        bool currentTxBit = false;
        bool error = false;
        
        // Start bit-banging RX/TX pins. Arbitration detection is done by checking if written bit is the same as the received bit.
        // Check RX-pin once again to be sure it's idling.
        currentRxBit = (RX_PIN & (1 << RX_P));
        if (!currentRxBit) error = true; // it's supposed to be logic high, another module is ahead of us
        
        // Write/read start bit (0 bit).
        if (!error)
        {
            cbi(TX_PORT, TX_P);
            _delay_us(64.0); // wait 0.5 bit time (at 7812.5 baud it's 64 microseconds)
            currentRxBit = (RX_PIN & (1 << RX_P)); // read RX pin state (logic high or low)
            if (currentRxBit) error = true; // it's supposed to be logic low
            _delay_us(64.0); // wait another 0.5 bit time to finish start bit signaling
        }
        
        // Write/read 8 data bits.
        for (uint8_t i = 0; i < 8; i++)
        {
            if (!error) // if bus arbitration is lost, don't write anything, just read bits
            {
                currentTxBit = IDbyteTX & (1 << i);
                if (currentTxBit) sbi(TX_PORT, TX_P);
                else cbi(TX_PORT, TX_P);
            }
            
            _delay_us(64.0); // wait 0.5 bit time
            currentRxBit = (RX_PIN & (1 << RX_P)); // read RX pin state
            if (currentRxBit) sbi(IDbyteRX, i); // save bit
            if (currentRxBit != currentTxBit) error = true;// error: bit mismatch, bus arbitration lost
            _delay_us(64.0); // wait another 0.5 bit time to finish this bit
        }
        
        // Write/read stop bit (1 bit).
        if (!error) sbi(TX_PORT, TX_P); // write stop bit at TX pin (1 bit)
        _delay_us(64.0); // wait 0.5 bit time (at 7812.5 baud it's 64 microseconds)
        currentRxBit = (RX_PIN & (1 << RX_P)); // read RX pin state (logic high or low)
        if (!currentRxBit) error = true; // error: it's supposed to be logic high
        _delay_us(64.0); // wait another 0.5 bit time to finish stop bit signaling
        
        // Start bus idle timer.
        busIdleTimerStart();
        
        // Save ID byte
        _serialRxBuffer[0] = IDbyteRX;
        _serialRxBufferPos = 1;
        
        if (!error && (IDbyteRX == IDbyteTX))
        {
            // If every bit in the ID-byte is echoed back correctly then we won bus arbitration
            // and we can continue to send the rest of the message bytes.
            
            // Save the ID-byte manually in the serial receive buffer.
            _serialRxBuffer[0] = IDbyteRX;
            _serialRxBufferPos = 1;
            
            // Continue automatic transmission at the second byte.
            _serialTxBufferPos = 1;
            
            // Re-enable UART receiver and transmitter and receive complete interrupt.
            UCSR1B |= (1 << RXCIE1) | (1 << RXEN1) | (1 << TXEN1);
            
            // Enable UDRE interrupt to continue message transmission.
            UCSR1B |= (1 << UDRIE1);
            
            return 0;
        }
        else
        {
            // Bus arbitration is lost somewhere along the way, this is an unexpected ID-byte. 
            // Save it and continue receiving message bytes.
            _serialRxBuffer[0] = IDbyteRX;
            _serialRxBufferPos = 1;
            
            // Reset transmit buffer.
            _serialTxBufferPos = 0;
            _serialTxLength = 0;
            
            // Re-enable UART receiver and transmitter and receive complete interrupt.
            UCSR1B |= (1 << RXCIE1) | (1 << RXEN1) | (1 << TXEN1);
            
            return 3;
        }
    }
}

void CCDLibrary::processMessage()
{
    // Check if there is something in the buffer.
    if (_serialRxBufferPos > 0)
    {
        if (_verifyRxChecksum && (_serialRxBufferPos > 1)) // verify checksum
        {
            uint8_t checksum = 0;
            uint8_t checksumLocation = _serialRxBufferPos - 1;
            for (uint8_t i = 0; i < checksumLocation ; i++) checksum += _serialRxBuffer[i];
            
            if (checksum == _serialRxBuffer[checksumLocation])
            {
                // Copy bytes from serial receive buffer to message buffer.
                for (uint8_t i = 0; i < _serialRxBufferPos; i++) _message[i] = _serialRxBuffer[i];
                
                _messageLength = _serialRxBufferPos;
                _serialRxBufferPos = 0;
            }
            else
            {
                _messageLength = 0; // let invalid messages have zero length
                _serialRxBufferPos = 0; // ignore this message and reset buffer
            }
        }
        else // checksum calculation is not applicable
        {
            // Copy bytes from serial receive buffer to message buffer.
            for (uint8_t i = 0; i < _serialRxBufferPos; i++) _message[i] = _serialRxBuffer[i];
            
            _messageLength = _serialRxBufferPos;
            _serialRxBufferPos = 0;
        }
        _lastMessageRead = false; // clear flag
    }
}

ISR(TIMER3_COMPA_vect)
{
    CCD.handle_TIMER3_COMPA_vect();
}

void CCDLibrary::handle_TIMER3_COMPA_vect()
{
    busIdleTimerStop(); // stop bus idle timer
    _busIdle = true; // set flag
    processMessage(); // process received message
}

ISR(USART1_RX_vect)
{
    CCD.handle_USART1_RX_vect();
}

void CCDLibrary::handle_USART1_RX_vect()
{
    if (!_interruptsAvailable) busIdleTimerStart(); // start bus idle timer immediately after the stop bit
    _busIdle = false; // TODO: clear flag after first bit received, not after a whole byte is received
    
    // Read UART status register and UART data register.
    uint8_t usr  = UCSR1A;
    uint8_t data = UDR1;
    
    // Get error bits from status register.
    uint8_t lastRxError = (usr & ((1 << FE1) | (1 << DOR1)));
    
    // Save byte in serial receive buffer.
    if (_serialRxBufferPos < 16)
    {
        _serialRxBuffer[_serialRxBufferPos] = data;
        _serialRxBufferPos++;
    }
    else
    {
        lastRxError |= UART_BUFFER_OVERFLOW; // error: buffer overflow
    }
    
    // Save last serial error.
    _lastSerialError = lastRxError;
}

ISR(USART1_UDRE_vect)
{
    CCD.handle_USART1_UDRE_vect();
}

void CCDLibrary::handle_USART1_UDRE_vect()
{
    if (_serialTxBufferPos < _serialTxLength)
    {
        UDR1 = _serialTxBuffer[_serialTxBufferPos]; // write next byte
        _serialTxBufferPos++; // increment Tx buffer position
    }
    else
    {
        UCSR1B &= ~(1 << UDRIE1); // Tx buffer empty, disable UDRE interrupt
        _serialTxBufferPos = 0; // reset Tx buffer position
        _serialTxLength = 0; // reset length
    }
}

void CCDLibrary::serialInit(uint16_t ubrr)
{
    // Reset buffer.
    ATOMIC_BLOCK(ATOMIC_FORCEON)
    {
        _serialRxBufferPos = 0;
        _serialTxBufferPos = 0;
        _serialTxLength = 0;
        _lastSerialError = 0;
    }
    
    // Set baud rate.
    UBRR1H = (ubrr >> 8) & 0x0F;
    UBRR1L = ubrr & 0xFF;
    
    // Enable UART receiver and transmitter and receive complete interrupt.
    UCSR1B |= (1 << RXCIE1) | (1 << RXEN1) | (1 << TXEN1);
    
    // Set frame format: asynchronous, 8 data bit, no parity, 1 stop bit.
    UCSR1C |= (1 << UCSZ10) | (1 << UCSZ11);
}

void CCDLibrary::busIdleTimerInit()
{
    // Calculate top value to count.
    // OCR3A = ((F_CPU * (1 / BAUDRATE) * BIT_DELAY) / PRESCALER) - 1
    //    F_CPU = 16000000 Hz
    //    BAUDRATE = 7812.5 bits per second
    //    PRESCALER = 1024
    // OCR3A (10 bits delay) = ((16000000 * (1 / 7812.5) * 10) / 1024) - 1 = 19
    // OCR3A (11 bits delay) = ((16000000 * (1 / 7812.5) * 10) / 1024) - 1 = 21
    // OCR3A (12 bits delay) = ((16000000 * (1 / 7812.5) * 10) / 1024) - 1 = 23
    // OCR3A (13 bits delay) = ((16000000 * (1 / 7812.5) * 13) / 1024) - 1 = 25
    // OCR3A (14 bits delay) = ((16000000 * (1 / 7812.5) * 10) / 1024) - 1 = 27
    _calculatedOCRAValue = (uint8_t)((((float)F_CPU * (1.0 / 7812.5) * (float)_busIdleBits) / 1024.0) - 1.0);
    
    // Setup Timer 3 to do idle timing measurements.
    noInterrupts();
    TCCR3A = 0;
    TCCR3B = 0;
    TCNT3 = 0;
    OCR3A = 0;
    TIMSK3 |= (1 << OCIE3A); // output Compare Match A Interrupt Enable
    interrupts();
}

void CCDLibrary::busIdleTimerStart()
{
    TCCR3B |= (1 << WGM32); // CTC
    TCCR3B |= (1 << CS32) | (1 << CS30); // prescaler: 1024
    TCNT3 = 0;
    OCR3A = _calculatedOCRAValue;
}

void CCDLibrary::busIdleTimerStop()
{
    TCCR3A = 0;
    TCCR3B = 0;
    TCNT3 = 0;
    OCR3A = 0;
}

void CCDLibrary::busIdleInterruptHandler()
{
    _busIdle = true; // set flag
    processMessage(); // process received message
}

void CCDLibrary::activeByteInterruptHandler()
{
    _busIdle = false;
}