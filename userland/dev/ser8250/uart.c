/*
 * uart.c — 16550 register-level driver. See uart.h for register
 * map. v0.6.1 does 8N1 raw mode; cooked-mode line discipline is
 * deferred. TX is polled (cheap thanks to the 16-byte FIFO).
 */

#include "uart.h"
#include "ring.h"

void uart_init(void)
{
    /* Disable all UART interrupts during setup. */
    uart_reg_write(UART_IER, 0);

    /* DLAB on to access the baud-rate divisor. QEMU virt is
     * permissive about the divisor; we set 1 just to be defined. */
    uart_reg_write(UART_LCR, LCR_BAUD_LATCH);
    uart_reg_write(0, 1);     /* divisor low */
    uart_reg_write(1, 0);     /* divisor high */

    /* DLAB off, 8 data bits, no parity, 1 stop bit. */
    uart_reg_write(UART_LCR, LCR_8N1);

    /* Enable + clear FIFOs. */
    uart_reg_write(UART_FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

    /* Drain any stale status. */
    (void)uart_reg_read(UART_LSR);
    (void)uart_reg_read(UART_RHR);
    (void)uart_reg_read(UART_ISR);

    /* Route interrupts to the PLIC (OUT2 strap). */
    uart_reg_write(UART_MCR, MCR_OUT2);

    /* Enable RX-data interrupts. (TX is polled in v0.6.1.) */
    uart_reg_write(UART_IER, IER_RX_DATA);
}

void uart_tx_byte(unsigned char b)
{
    /* Spin until the transmitter holding register is empty.
     * On QEMU virt this completes nearly instantly. */
    while ((uart_reg_read(UART_LSR) & LSR_TX_IDLE) == 0) {
        /* spin */
    }
    uart_reg_write(UART_THR, b);
}

unsigned uart_drain_rx(struct ser_ring *r)
{
    unsigned n = 0;
    while (uart_reg_read(UART_LSR) & LSR_RX_READY) {
        unsigned char b = uart_reg_read(UART_RHR);
        /* v0.7-rc3: no driver-side echo or CR/NL fiddling.  Line
         * discipline lives in libqsoe's qsoe_ldisc_* layer; the
         * driver just passes raw bytes through. */
        if (ser_ring_push(r, b) != 0) break;  /* ring full; drop further */
        ++n;
    }
    return n;
}
