#include "touch.h"
#include "display_constants.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>          // abs
#include <cstring>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

// XPT2046 commands: start bit, channel, 12-bit, differential, PD = 00 (power down
// between conversions, which keeps the pen IRQ enabled). Same as test_touch.py.
constexpr unsigned char READ_X = 0xD0;
constexpr unsigned char READ_Y = 0x90;

bool touch_panel::open() {
    spi = ::open(TOUCH_SPI, O_RDWR);
    if (spi < 0) {
        perror(TOUCH_SPI);
        return false;
    }
    unsigned char mode = SPI_MODE_0, bits = 8;
    uint32_t speed = TOUCH_SPI_HZ;
    if (ioctl(spi, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(spi, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spi, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("touch SPI setup");
        close();
        return false;
    }

    // T_IRQ as an input; the pull-up comes from config.txt
    chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) {
        std::fprintf(stderr, "gpiod_chip_open %s: %s\n", GPIO_CHIP, std::strerror(errno));
        close();
        return false;
    }
    gpiod_line_settings* settings = gpiod_line_settings_new();
    gpiod_line_config* line_cfg = gpiod_line_config_new();
    gpiod_request_config* req_cfg = gpiod_request_config_new();
    if (settings && line_cfg && req_cfg) {
        gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
        unsigned int offset = TOUCH_IRQ_GPIO;
        if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings) == 0) {
            gpiod_request_config_set_consumer(req_cfg, "thermostat-display");
            irq = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
        }
    }
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    if (!irq) {
        perror("touch IRQ line (GPIO 16)");
        close();
        return false;
    }
    return true;
}

void touch_panel::close() {
    if (irq) gpiod_line_request_release(irq);
    if (chip) gpiod_chip_close(chip);
    if (spi >= 0) ::close(spi);
    irq = nullptr;
    chip = nullptr;
    spi = -1;
}

bool touch_panel::pen_down() {
    return gpiod_line_request_get_value(irq, TOUCH_IRQ_GPIO) == GPIOD_LINE_VALUE_INACTIVE;   // low
}

// one 12-bit conversion: command byte out, result in the next two bytes
int touch_panel::sample(unsigned char command) {
    unsigned char tx[3] = {command, 0, 0}, rx[3] = {0, 0, 0};
    spi_ioc_transfer t;
    std::memset(&t, 0, sizeof t);
    t.tx_buf = (unsigned long)tx;
    t.rx_buf = (unsigned long)rx;
    t.len = 3;
    t.speed_hz = TOUCH_SPI_HZ;
    t.bits_per_word = 8;
    if (ioctl(spi, SPI_IOC_MESSAGE(1), &t) < 0) return -1;   // filtered out as out of range
    return ((rx[1] << 8) | rx[2]) >> 3;
}

bool touch_panel::read(screen_point& point) {
    if (spi < 0 || !pen_down()) {
        have_first = settled = false;
        return false;
    }

    std::vector<int> xs, ys;
    for (int i = 0; i < TOUCH_SAMPLES; i++) {
        xs.push_back(sample(READ_X));
        ys.push_back(sample(READ_Y));
    }
    std::optional<int> x = filter_samples(xs, TOUCH_RAW_MIN, TOUCH_RAW_MAX, TOUCH_SAMPLES / 2 + 1, TOUCH_MAX_SPREAD);
    std::optional<int> y = filter_samples(ys, TOUCH_RAW_MIN, TOUCH_RAW_MAX, TOUCH_SAMPLES / 2 + 1, TOUCH_MAX_SPREAD);

    // lifted during the burst: its samples may be the junk from the lift
    if (!pen_down()) {
        have_first = settled = false;
        return false;
    }
    // a bad burst mid-press: hold the last good point rather than flicker to released
    if (!x || !y) {
        point = last;
        return settled;
    }

    if (!settled) {
        bool agrees = have_first && std::abs(*x - first_x) <= TOUCH_SETTLE_MOVE &&
                      std::abs(*y - first_y) <= TOUCH_SETTLE_MOVE;
        if (!agrees) {
            have_first = true;
            first_x = *x;
            first_y = *y;
            return false;
        }
        settled = true;
    }
    last = touch_to_screen(*x, *y, TOUCH_CAL);
    point = last;
    return true;
}
