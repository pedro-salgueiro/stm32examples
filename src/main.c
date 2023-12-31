#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/systick.h>

/*
 * To implement the STDIO functions you need to create
 * the _read and _write functions and hook them to the
 * USART you are using. This example also has a buffered
 * read function for basic line editing.
 */
int _write(int fd, char *ptr, int len);
int _read(int fd, char *ptr, int len);
void get_buffered_line(void);

/*
 * This is a pretty classic ring buffer for characters
 */
#define BUFLEN 127

static uint16_t start_ndx;
static uint16_t end_ndx;
static char buf[BUFLEN + 1];
#define buf_len ((end_ndx - start_ndx) % BUFLEN)
static inline int inc_ndx(int n) { return ((n + 1) % BUFLEN); }
static inline int dec_ndx(int n) { return (((n + BUFLEN) - 1) % BUFLEN); }

volatile uint32_t system_millis;
void sys_tick_handler(void)
{
	system_millis++;
}

static void msleep(uint32_t delay)
{
	uint32_t wake = system_millis + delay;
	while (wake > system_millis)
		;
}

static void systick_setup(void)
{
	/* clock rate / 1000 to get 1mS interrupt rate */
	systick_set_reload(96000);
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
	systick_counter_enable();
	/* this done last */
	systick_interrupt_enable();
}

/* Set STM32 to 168 MHz. */
static void clock_setup(void)
{
	rcc_clock_setup_pll(&rcc_hse_8mhz_3v3[RCC_CLOCK_3V3_168MHZ]);

	/* Enable GPIOD clock. */
	rcc_periph_clock_enable(RCC_GPIOD);
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_USART2);	
}

static void usart_setup(void)
{
	/* Setup USART2 parameters. */
	usart_set_baudrate(USART2, 115200);
	usart_set_databits(USART2, 8);
	usart_set_stopbits(USART2, USART_STOPBITS_1);
	usart_set_mode(USART2, USART_MODE_TX_RX);
	usart_set_parity(USART2, USART_PARITY_NONE);
	usart_set_flow_control(USART2, USART_FLOWCONTROL_NONE);

	/* Finally enable the USART. */
	usart_enable(USART2);
}

static void gpio_setup(void)
{
	gpio_mode_setup(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GPIO1);

	/* Setup GPIO pins for USART2 transmit. */
	gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO2);

	/* Setup USART2 TX pin as alternate function. */
	gpio_set_af(GPIOA, GPIO_AF7, GPIO2);
}

int main(void)
{
	clock_setup();
	systick_setup();
	gpio_setup();
	usart_setup();

	gpio_set(GPIOA, GPIO1);

	while (1)
	{
		gpio_toggle(GPIOA, GPIO1);
		msleep(1000);

		printf("Tic-tac %ld\n", system_millis);
	}

	return 0;
}

/* back up the cursor one space */
static inline void back_up(void)
{
	end_ndx = dec_ndx(end_ndx);
	usart_send_blocking(USART2, '\010');
	usart_send_blocking(USART2, ' ');
	usart_send_blocking(USART2, '\010');
}

/*
 * A buffered line editing function.
 */
void get_buffered_line(void)
{
	char c;

	if (start_ndx != end_ndx)
	{
		return;
	}
	while (1)
	{
		c = usart_recv_blocking(USART2);
		if (c == '\r')
		{
			buf[end_ndx] = '\n';
			end_ndx = inc_ndx(end_ndx);
			buf[end_ndx] = '\0';
			usart_send_blocking(USART2, '\r');
			usart_send_blocking(USART2, '\n');
			return;
		}
		/* ^H or DEL erase a character */
		if ((c == '\010') || (c == '\177'))
		{
			if (buf_len == 0)
			{
				usart_send_blocking(USART2, '\a');
			}
			else
			{
				back_up();
			}
			/* ^W erases a word */
		}
		else if (c == 0x17)
		{
			while ((buf_len > 0) &&
				   (!(isspace((int)buf[end_ndx]))))
			{
				back_up();
			}
			/* ^U erases the line */
		}
		else if (c == 0x15)
		{
			while (buf_len > 0)
			{
				back_up();
			}
			/* Non-editing character so insert it */
		}
		else
		{
			if (buf_len == (BUFLEN - 1))
			{
				usart_send_blocking(USART2, '\a');
			}
			else
			{
				buf[end_ndx] = c;
				end_ndx = inc_ndx(end_ndx);
				usart_send_blocking(USART2, c);
			}
		}
	}
}

/*
 * Called by libc stdio fwrite functions
 */
int _write(int fd, char *ptr, int len)
{
	int i = 0;

	/*
	 * Write "len" of char from "ptr" to file id "fd"
	 * Return number of char written.
	 *
	 * Only work for STDOUT, STDIN, and STDERR
	 */
	if (fd > 2)
	{
		return -1;
	}
	while (*ptr && (i < len))
	{
		usart_send_blocking(USART2, *ptr);
		if (*ptr == '\n')
		{
			usart_send_blocking(USART2, '\r');
		}
		i++;
		ptr++;
	}
	return i;
}

/*
 * Called by the libc stdio fread fucntions
 *
 * Implements a buffered read with line editing.
 */
int _read(int fd, char *ptr, int len)
{
	int my_len;

	if (fd > 2)
	{
		return -1;
	}

	get_buffered_line();
	my_len = 0;
	while ((buf_len > 0) && (len > 0))
	{
		*ptr++ = buf[start_ndx];
		start_ndx = inc_ndx(start_ndx);
		my_len++;
		len--;
	}
	return my_len; /* return the length we got */
}