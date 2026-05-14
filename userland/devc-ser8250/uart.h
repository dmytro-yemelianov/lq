/*
 * uart.h — 16550 UART register layout + helpers.
 * QEMU virt UART lives at physical 0x10000000. spawn.c maps it into
 * the driver's VSpace at UART_VADDR (= 0xA00000). All access is via
 * volatile MMIO reads/writes on byte-wide registers.
 */
#ifndef QSOE_DEVC_SER8250_UART_H
#define QSOE_DEVC_SER8250_UART_H

#define UART_VADDR 0xA00000UL

/* Register offsets (DLAB=0). With DLAB=1, reg 0 is divisor low and
 * reg 1 is divisor high. */
#define UART_RHR  0  /* RX holding (read) */
#define UART_THR  0  /* TX holding (write) */
#define UART_IER  1  /* Interrupt Enable */
#define UART_ISR  2  /* Interrupt Status (read) */
#define UART_FCR  2  /* FIFO Control (write) */
#define UART_LCR  3  /* Line Control */
#define UART_MCR  4  /* Modem Control */
#define UART_LSR  5  /* Line Status */

/* IER bits. */
#define IER_RX_DATA  0x01
#define IER_TX_EMPTY 0x02

/* LCR bits. */
#define LCR_8N1        0x03  /* 8 data bits, 1 stop, no parity */
#define LCR_BAUD_LATCH 0x80  /* DLAB on */

/* FCR bits. */
#define FCR_FIFO_ENABLE 0x01
#define FCR_FIFO_CLEAR  0x06  /* clear RX + TX FIFOs */

/* MCR bits. */
#define MCR_OUT2 0x08  /* OUT2: route IRQs to PLIC */

/* LSR bits. */
#define LSR_RX_READY 0x01
#define LSR_TX_IDLE  0x20

/* MMIO accessors — volatile so the compiler doesn't fold reads/writes. */
static inline unsigned char uart_reg_read(unsigned reg)
{
    return *(volatile unsigned char *)(UART_VADDR + reg);
}
static inline void uart_reg_write(unsigned reg, unsigned char v)
{
    *(volatile unsigned char *)(UART_VADDR + reg) = v;
}

/* One-time hardware init. Disables interrupts, sets 8N1 framing,
 * enables FIFOs, routes IRQs through OUT2, enables RX-data IRQ. */
void uart_init(void);

/* Polled TX: spin on LSR.TX_IDLE then write THR. */
void uart_tx_byte(unsigned char b);

/* Drain everything currently in the RX FIFO into the given ring buffer.
 * Returns the number of bytes pushed (0 if FIFO was empty). */
struct ser_ring;
unsigned uart_drain_rx(struct ser_ring *r);

#endif
