/****************************************************************************
 *   apps/rf_sub1G/simple/main.c
 *
 * sub1G_module support code - USB version
 *
 * Copyright 2013-2014 Nathael Pajani <nathael.pajani@ed3l.fr>
 *
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *************************************************************************** */

#include "core/system.h"
#include "core/systick.h"
#include "core/pio.h"
#include "lib/stdio.h"
#include "drivers/serial.h"
#include "drivers/gpio.h"
#include "drivers/ssp.h"
#include "extdrv/cc1101.h"
#include "extdrv/status_led.h"
#include "drivers/i2c.h"

#include "extdrv/bme280_humidity_sensor.h"
#include "extdrv/veml6070_uv_sensor.h"
#include "extdrv/tsl256x_light_sensor.h"
#include "extdrv/ssd130x_oled_driver.h"
#include "extdrv/ssd130x_oled_buffer.h"
#include "lib/font.h"

#define MODULE_VERSION	0x03
#define MODULE_NAME "RF Sub1G - USB"

#define RF_868MHz  1
#define RF_915MHz  0
#if ((RF_868MHz) + (RF_915MHz) != 1)
#error Either RF_868MHz or RF_915MHz MUST be defined.
#endif

#define DEBUG 1
#define BUFF_LEN 60
#define RF_BUFF_LEN  64

#define SELECTED_FREQ  FREQ_SEL_48MHz
#define DEVICE_ADDRESS  0x02 /* Addresses 0x00 and 0xFF are broadcast */
#define NEIGHBOR_ADDRESS 0x13 /* Address of the associated device */
/***************************************************************************** */
/* Pins configuration */
/* pins blocks are passed to set_pins() for pins configuration.
 * Unused pin blocks can be removed safely with the corresponding set_pins() call
 * All pins blocks may be safelly merged in a single block for single set_pins() call..
 */
const struct pio_config common_pins[] = {
	/* UART 0 */
	{ LPC_UART0_RX_PIO_0_1,  LPC_IO_DIGITAL },
	{ LPC_UART0_TX_PIO_0_2,  LPC_IO_DIGITAL },
	/* SPI */
	{ LPC_SSP0_SCLK_PIO_0_14, LPC_IO_DIGITAL },
	{ LPC_SSP0_MOSI_PIO_0_17, LPC_IO_DIGITAL },
	{ LPC_SSP0_MISO_PIO_0_16, LPC_IO_DIGITAL },
	/* I2C 0 */
	{ LPC_I2C0_SCL_PIO_0_10, (LPC_IO_DIGITAL | LPC_IO_OPEN_DRAIN_ENABLE) },
	{ LPC_I2C0_SDA_PIO_0_11, (LPC_IO_DIGITAL | LPC_IO_OPEN_DRAIN_ENABLE) },
	ARRAY_LAST_PIO,
};

const struct pio cc1101_cs_pin = LPC_GPIO_0_15;
const struct pio cc1101_miso_pin = LPC_SSP0_MISO_PIO_0_16;
const struct pio cc1101_gdo0 = LPC_GPIO_0_6;
const struct pio cc1101_gdo2 = LPC_GPIO_0_7;

const struct pio status_led_green = LPC_GPIO_0_28;
const struct pio status_led_red = LPC_GPIO_0_29;

const struct pio button = LPC_GPIO_0_12; /* ISP button */

// Envoi
struct message 
{
	uint32_t temp;
	uint16_t hum;
	uint32_t lum;
};
typedef struct message message;

//Reception
struct messageFormat 
{
	char format[3];
	uint32_t temp;
	uint16_t hum;
	uint32_t lum;
};
typedef struct messageFormat messageFormat;


/***************************************************************************** */
void system_init()
{
	/* Stop the watchdog */
	startup_watchdog_disable(); /* Do it right now, before it gets a chance to break in */
	system_set_default_power_state();
	clock_config(SELECTED_FREQ);
	set_pins(common_pins);
	gpio_on();
	/* System tick timer MUST be configured and running in order to use the sleeping
	 * functions */
	systick_timer_on(1); /* 1ms */
	systick_start();
}

/* Define our fault handler. This one is not mandatory, the dummy fault handler
 * will be used when it's not overridden here.
 * Note : The default one does a simple infinite loop. If the watchdog is deactivated
 * the system will hang.
 */
void fault_info(const char* name, uint32_t len)
{
	uprintf(UART0, name);
	while (1);
}

static volatile int check_rx = 0;
void rf_rx_calback(uint32_t gpio)
{
	check_rx = 1;
}

static uint8_t rf_specific_settings[] = {
	CC1101_REGS(gdo_config[2]), 0x07, /* GDO_0 - Assert on CRC OK | Disable temp sensor */
	CC1101_REGS(gdo_config[0]), 0x2E, /* GDO_2 - FIXME : do something usefull with it for tests */
	CC1101_REGS(pkt_ctrl[0]), 0x0F, /* Accept all sync, CRC err auto flush, Append, Addr check and Bcast */
#if (RF_915MHz == 1)
	/* FIXME : Add here a define protected list of settings for 915MHz configuration */
#endif
};

/***************************************************************************** */
/* Adafruit Oled Display */
/* #define DISPLAY_ADDR   0x7A */
/* For other OLED Displays maybe : 0x78*/
#define DISPLAY_ADDR   0x7A
static uint8_t gddram[ 4 + GDDRAM_SIZE ];
struct oled_display display = {
	.bus_type = SSD130x_BUS_I2C,
	.address = DISPLAY_ADDR,
	.bus_num = I2C0,
	.charge_pump = SSD130x_INTERNAL_PUMP,
	.gpio_rst = LPC_GPIO_0_0,
	.video_mode = SSD130x_DISP_NORMAL,
	.contrast = 128,
	.scan_dir = SSD130x_SCAN_BOTTOM_TOP,
	.read_dir = SSD130x_RIGHT_TO_LEFT,
	.display_offset_dir = SSD130x_MOVE_TOP,
	.display_offset = 4,
  .gddram = gddram,
};

#define ROW(x)   VERTICAL_REV(x)
DECLARE_FONT(font);

void display_char(uint8_t line, uint8_t col, uint8_t c)
{
	uint8_t tile = (c > FIRST_FONT_CHAR) ? (c - FIRST_FONT_CHAR) : 0;
	uint8_t* tile_data = (uint8_t*)(&font[tile]);
	ssd130x_buffer_set_tile(gddram, col, line, tile_data);
}

int display_line(uint8_t line, uint8_t col, char* text)
{
	int len = strlen((char*)text);
	int i = 0;

	for (i = 0; i < len; i++) {
		uint8_t tile = (text[i] > FIRST_FONT_CHAR) ? (text[i] - FIRST_FONT_CHAR) : 0;
		uint8_t* tile_data = (uint8_t*)(&font[tile]);
		ssd130x_buffer_set_tile(gddram, col++, line, tile_data);
		if (col >= (SSD130x_NB_COL / 8)) {
			col = 0;
			line++;
			if (line >= SSD130x_NB_PAGES) {
				return i;
			}
		}
	}
	return len;
}

/* RF config */
void rf_config(void)
{
	config_gpio(&cc1101_gdo0, LPC_IO_MODE_PULL_UP, GPIO_DIR_IN, 0);
	cc1101_init(0, &cc1101_cs_pin, &cc1101_miso_pin); /* ssp_num, cs_pin, miso_pin */
	/* Set default config */
	cc1101_config();
	/* And change application specific settings */
	cc1101_update_config(rf_specific_settings, sizeof(rf_specific_settings));
	set_gpio_callback(rf_rx_calback, &cc1101_gdo0, EDGE_RISING);
    cc1101_set_address(DEVICE_ADDRESS);
#ifdef DEBUG
	uprintf(UART0, "CC1101 RF link init done.\n\r");
#endif
}


uint8_t chenillard_active = 1;
void handle_rf_rx_data(void)
{
	messageFormat msg;
	uint8_t data[RF_BUFF_LEN];
	char data_oled[20];
	int8_t ret = 0;
	uint8_t status = 0;

	/* Check for received packet (and get it if any) */
	ret = cc1101_receive_packet(data, RF_BUFF_LEN, &status);
	/* Go back to RX mode */
	cc1101_enter_rx_mode();

	memcpy(&msg, &data[2], sizeof(messageFormat));

	if (data[0] == 17) {

#ifdef DEBUG
	uprintf(UART0, "Reception: ret:%d, st: %d\n\r", ret, status);
    uprintf(UART0, "Reception: data length: %d\n\r", data[0]);
    uprintf(UART0, "Reception: destination: %x\n\r", data[1]);
    uprintf(UART0, "Reception: format: %s\n\r", msg.format);
    uprintf(UART0, "Reception: temp: %d\n\r", msg.temp);
    uprintf(UART0, "Reception: hum: %d\n\r", msg.hum);
    uprintf(UART0, "Reception: lum: %d\n\r", msg.lum);
#endif

	ssd130x_buffer_set(gddram, 0x00);
	int i;
	int n = 2;
	for (i = 0; i < 3; i++) {
		char format = msg.format[i];
		if (format == 'T') {
			snprintf(data_oled, 20, "T: %d,%d degC", (msg.temp / 10), (msg.temp % 10));
			display_line(n, 0, data_oled);
		} else if (format == 'H') {
			snprintf(data_oled, 20, "H: %d,%d rH", msg.hum / 10, msg.hum % 10);
			display_line(n, 0, data_oled);
		} else if (format == 'L') {
			snprintf(data_oled, 20, "L: %d", msg.lum);
			display_line(n, 0, data_oled);
		}
		n++;
	}
	ret = ssd130x_display_full_screen(&display);
	if (ret < 0) {
		uprintf(UART0, "Display update error: %d\n\r", ret);
	}
	}
}

static volatile uint32_t cc_tx = 0;
static volatile uint8_t cc_tx_buff[RF_BUFF_LEN];
static volatile uint8_t cc_ptr = 0;
uint8_t chenillard_activation_request = 1;
void activate_chenillard(uint32_t gpio) {
	if (chenillard_activation_request == 1){
        cc_tx_buff[0]='0';
        cc_ptr = 1;
        cc_tx=1;
        chenillard_activation_request = 0;
    }
    else{
        cc_tx_buff[0]='1';
        cc_ptr = 1;
        cc_tx=1;
        chenillard_activation_request = 1;
    }
}

/***************************************************************************** */
/* Luminosity */

/* Note : These are 8bits address */
#define TSL256x_ADDR   0x52 /* Pin Addr Sel (pin2 of tsl256x) connected to GND */
struct tsl256x_sensor_config tsl256x_sensor = {
	.bus_num = I2C0,
	.addr = TSL256x_ADDR,
	.gain = TSL256x_LOW_GAIN,
	.integration_time = TSL256x_INTEGRATION_100ms,
	.package = TSL256x_PACKAGE_T,
};

void lux_config(int uart_num)
{
	int ret = 0;
	ret = tsl256x_configure(&tsl256x_sensor);
	if (ret != 0) {
		uprintf(uart_num, "Lux config error: %d\n\r", ret);
	}
}

void lux_display(int uart_num, uint16_t* ir, uint32_t* lux)
{
	uint16_t comb = 0;
	int ret = 0;

	ret = tsl256x_sensor_read(&tsl256x_sensor, &comb, ir, lux);
	if (ret != 0) {
		uprintf(uart_num, "Lux read error: %d\n\r", ret);
	} else {
		uprintf(uart_num, "Lux: %d  (Comb: 0x%04x, IR: 0x%04x)\n\r", *lux, comb, *ir);
	}
}

/***************************************************************************** */
/* BME280 Sensor */

/* Note : 8bits address */
#define BME280_ADDR   0xEC
struct bme280_sensor_config bme280_sensor = {
	.bus_num = I2C0,
	.addr = BME280_ADDR,
	.humidity_oversampling = BME280_OS_x16,
	.temp_oversampling = BME280_OS_x16,
	.pressure_oversampling = BME280_OS_x16,
	.mode = BME280_NORMAL,
	.standby_len = BME280_SB_62ms,
	.filter_coeff = BME280_FILT_OFF,
};

void bme_config(int uart_num)
{
	int ret = 0;

	ret = bme280_configure(&bme280_sensor);
	if (ret != 0) {
		uprintf(uart_num, "Sensor config error: %d\n\r", ret);
	}
}

/* BME will obtain temperature, pressure and humidity values */

void bme_display(int uart_num, uint32_t* pressure, uint32_t* temp, uint16_t* humidity)
{
	int ret = 0;

	ret = bme280_sensor_read(&bme280_sensor, pressure, temp, humidity);
	if (ret != 0) {
		uprintf(uart_num, "Sensor read error: %d\n\r", ret);
	} else {
		int comp_temp = 0;
		uint32_t comp_pressure = 0;
		uint32_t comp_humidity = 0;

		comp_temp = bme280_compensate_temperature(&bme280_sensor, *temp) / 10;
		comp_pressure = bme280_compensate_pressure(&bme280_sensor, *pressure) / 100;
		comp_humidity = bme280_compensate_humidity(&bme280_sensor, *humidity) / 10;
		uprintf(uart_num, "P: %d hPa, T: %d,%02d degC, H: %d,%d rH\n\r",
				comp_pressure,
				comp_temp / 10,  (comp_temp > 0) ? (comp_temp % 10) : ((-comp_temp) % 10),
				comp_humidity / 10, comp_humidity % 10);
		*temp = comp_temp;
		*pressure = comp_pressure;
		*humidity = comp_humidity;
	}
}

/***************************************************************************** */
/* UV */

/* The I2C UV light sensor is at addresses 0x70, 0x71 and 0x73 */
/* Note : These are 8bits address */
#define VEML6070_ADDR   0x70
struct veml6070_sensor_config veml6070_sensor = {
	.bus_num = I2C0,
	.addr = VEML6070_ADDR,
};

void uv_config(int uart_num)
{
	int ret = 0;

	/* UV sensor */
	ret = veml6070_configure(&veml6070_sensor);
	if (ret != 0) {
		uprintf(uart_num, "UV config error: %d\n\r", ret);
	}
	
}

void uv_display(int uart_num, uint16_t* uv_raw)
{
	int ret = 0;

	ret = veml6070_sensor_read(&veml6070_sensor, uv_raw);
	if (ret != 0) {
		uprintf(uart_num, "UV read error: %d\n\r", ret);
	} else {
		uprintf(uart_num, "UV: 0x%04x\n\r", *uv_raw);
	}
}

static volatile uint32_t update_display = 0;

/***************************************************************************** */
void periodic_display(uint32_t tick)
{
	update_display = 1;
}

static volatile message cc_tx_msg;
void send_on_rf(void)
{
	message data;
	uint8_t cc_tx_data[sizeof(message)+2];
	cc_tx_data[0]=sizeof(message)+1;
	cc_tx_data[1]=NEIGHBOR_ADDRESS;
	data.hum=cc_tx_msg.hum;
	data.lum=cc_tx_msg.lum;
	data.temp=cc_tx_msg.temp;
	memcpy(&cc_tx_data[2], &data, sizeof(message));

	/* Send */
	if (cc1101_tx_fifo_state() != 0) {
		cc1101_flush_tx_fifo();
	}

	int ret = cc1101_send_packet(cc_tx_data, sizeof(message)+2);

#ifdef DEBUG
	uprintf(UART0, "\n\r*********************\n\r");
	uprintf(UART0, "Emission: ret: %d\n\r", ret);
    uprintf(UART0, "Emission: data length: %d.\n\r", cc_tx_data[0]);
    uprintf(UART0, "Emission: destination: %x.\n\r", cc_tx_data[1]);
    uprintf(UART0, "Emission: temp: %d\n\r", data.temp);
    uprintf(UART0, "Emission: hum: %d\n\r", data.hum);
    uprintf(UART0, "Emission: lum: %d\n\r", data.lum);
#endif
}


int main(void)
{
	system_init();
	uart_on(UART0, 115200, NULL);
	i2c_on(I2C0, I2C_CLK_100KHz, I2C_MASTER);
	ssp_master_on(0, LPC_SSP_FRAME_SPI, 8, 4*1000*1000); /* bus_num, frame_type, data_width, rate */
	status_led_config(&status_led_green, &status_led_red);
	
	/* Sensors config */
	uprintf(UART0, "Config Sensors\n\r");
	uprintf(UART0, "Config UV\n\r");
	uv_config(UART0);
	uprintf(UART0, "Config LUX\n\r");
	lux_config(UART0);
	uprintf(UART0, "Config BME\n\r");
	bme_config(UART0);

	/* Radio */
	rf_config();

	/* Configure and start display */
	ssd130x_display_on(&display);


	add_systick_callback(periodic_display, 1000);

    /* Activate the chenillard on Rising edge (button release) */
	set_gpio_callback(activate_chenillard, &button, EDGE_RISING);

	uprintf(UART0, "App started\n\r");

	while (1) {
		uint8_t status = 0;

		
        /* Verify that chenillard is enable */
        if (chenillard_active == 1) {
			/* Tell we are alive :) */
		    chenillard(250);
        }
        else{
            status_led(none);
			msleep(250);
        }

		if (update_display == 1) {
			uint16_t uv = 0, ir = 0, humidity = 0;
			uint32_t pressure = 0, temp = 0, lux = 0;
			
			/* Read the sensors */
			uv_display(UART0, &uv);
			bme_display(UART0, &pressure, &temp, &humidity);
			lux_display(UART0, &ir, &lux);
			cc_tx_msg.temp=temp;
			cc_tx_msg.hum=humidity;
			cc_tx_msg.lum=lux;
			update_display = 0;

			send_on_rf();

		}

		/* RF */
		if (cc_tx == 1) {
			cc_tx = 0;
		}
		/* Do not leave radio in an unknown or unwated state */
		do {
			status = (cc1101_read_status() & CC1101_STATE_MASK);
		} while (status == CC1101_STATE_TX);

		if (status != CC1101_STATE_RX) {
			static uint8_t loop = 0;
			loop++;
			if (loop > 10) {
				if (cc1101_rx_fifo_state() != 0) {
					cc1101_flush_rx_fifo();
				}
				cc1101_enter_rx_mode();
				loop = 0;
			}
		}
		if (check_rx == 1) {
			check_rx = 0;
			handle_rf_rx_data();
		}

		
	}
	return 0;
}
