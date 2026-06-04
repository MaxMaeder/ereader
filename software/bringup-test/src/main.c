/*
 * E-Reader bringup: load BMP pages from SD card and display on dual EPD.
 *
 * Pages are stored as page_0000.bmp through page_NNNN.bmp (960x552, 1-bit B/W).
 * Displays are mounted vertically (90° CW) so the 960-pixel hardware width
 * runs top-to-bottom physically. The BMP images are pre-rotated for this
 * orientation, so pixel data is written directly to the display.
 *
 * A "spread" is two consecutive pages: even page on left display, odd on right.
 * Currently only the left display is enabled.
 *
 * Button 2: next spread
 * Button 0: previous spread
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/input/input.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <string.h>

#define EPD_WIDTH     960
#define EPD_HEIGHT    552
#define EPD_ROW_BYTES (EPD_WIDTH / 8)
#define EPD_BUF_SIZE  (EPD_ROW_BYTES * EPD_HEIGHT)

#define PAGES_PER_SPREAD 2

static const struct device *leds;
static const struct device *epd_l;
static const struct device *epd_r;
static const struct device *sd_ldsw;
static bool sd_on;

static int current_spread;
static int total_pages;

enum button_action {
	BTN_NONE,
	BTN_NEXT,
	BTN_PREV,
};

static enum button_action pending_action;
static K_SEM_DEFINE(button_sem, 0, 1);

static uint8_t framebuf[EPD_BUF_SIZE];

static void touch_event_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || !evt->value) {
		return;
	}

	if (evt->code == INPUT_KEY_2) {
		pending_action = BTN_NEXT;
		k_sem_give(&button_sem);
	} else if (evt->code == INPUT_KEY_0) {
		pending_action = BTN_PREV;
		k_sem_give(&button_sem);
	}
}

INPUT_CALLBACK_DEFINE(NULL, touch_event_cb, NULL);

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = "/SD:",
};

static bool sd_power_on(void)
{
	int ret;

	if (sd_on) {
		return true;
	}

	if (!sd_ldsw) {
		return false;
	}

	ret = regulator_enable(sd_ldsw);
	if (ret && ret != -EALREADY) {
		printk("Failed to enable SD power (%d)\n", ret);
		return false;
	}
	k_sleep(K_MSEC(100));

	ret = disk_access_init("SD");
	if (ret) {
		printk("SD init failed (%d)\n", ret);
		return false;
	}

	ret = fs_mount(&mp);
	if (ret) {
		printk("FS mount failed (%d)\n", ret);
		return false;
	}

	sd_on = true;
	return true;
}

static void sd_power_off(void)
{
	if (!sd_on || !sd_ldsw) {
		return;
	}

	fs_unmount(&mp);
	regulator_disable(sd_ldsw);
	sd_on = false;
}

static bool page_exists(int page_num)
{
	char path[32];
	struct fs_dirent entry;

	snprintf(path, sizeof(path), "/SD:/page_%04d.bmp", page_num);
	return fs_stat(path, &entry) == 0;
}

static int load_bmp_page(int page_num)
{
	char path[32];
	struct fs_file_t f;
	uint8_t header[62];
	uint32_t data_offset;
	int32_t bmp_width, bmp_height;
	bool top_down = false;
	int row_bytes;
	int ret;

	snprintf(path, sizeof(path), "/SD:/page_%04d.bmp", page_num);

	fs_file_t_init(&f);
	ret = fs_open(&f, path, FS_O_READ);
	if (ret) {
		printk("Cannot open %s (%d)\n", path, ret);
		return ret;
	}

	ret = fs_read(&f, header, sizeof(header));
	if (ret < 26) {
		fs_close(&f);
		return -EIO;
	}

	data_offset = header[10] | (header[11] << 8) |
		      (header[12] << 16) | (header[13] << 24);
	bmp_width = (int32_t)(header[18] | (header[19] << 8) |
			      (header[20] << 16) | (header[21] << 24));
	bmp_height = (int32_t)(header[22] | (header[23] << 8) |
			       (header[24] << 16) | (header[25] << 24));

	if (bmp_height < 0) {
		bmp_height = -bmp_height;
		top_down = true;
	}

	if (bmp_width != EPD_WIDTH || bmp_height != EPD_HEIGHT) {
		printk("BMP size mismatch: %dx%d\n", bmp_width, bmp_height);
		fs_close(&f);
		return -EINVAL;
	}

	row_bytes = ((bmp_width + 31) / 32) * 4;

	fs_seek(&f, data_offset, FS_SEEK_SET);

	if (top_down) {
		for (int y = 0; y < EPD_HEIGHT; y++) {
			ret = fs_read(&f, &framebuf[y * EPD_ROW_BYTES],
				      row_bytes);
			if (ret < row_bytes) {
				break;
			}
		}
	} else {
		for (int y = EPD_HEIGHT - 1; y >= 0; y--) {
			ret = fs_read(&f, &framebuf[y * EPD_ROW_BYTES],
				      row_bytes);
			if (ret < row_bytes) {
				break;
			}
		}
	}

	fs_close(&f);
	return 0;
}

static int write_page_to_epd(const struct device *epd, int page_num)
{
	struct display_buffer_descriptor desc = {
		.buf_size = EPD_BUF_SIZE,
		.width = EPD_WIDTH,
		.height = EPD_HEIGHT,
		.pitch = EPD_WIDTH,
	};

	if (load_bmp_page(page_num)) {
		printk("Failed to load page %d\n", page_num);
		return -EIO;
	}

	display_blanking_on(epd);
	if (display_write(epd, 0, 0, &desc, framebuf)) {
		printk("Display write failed for page %d\n", page_num);
		return -EIO;
	}
	return 0;
}

static void show_spread(void)
{
	int left_page = current_spread * PAGES_PER_SPREAD;
	int right_page = left_page + 1;

	if (!sd_power_on()) {
		printk("SD card unavailable\n");
		return;
	}

	printk("Showing spread %d (pages %d-%d)\n",
	       current_spread, left_page, right_page);

	/* Write data to both displays while SD is on */
	write_page_to_epd(epd_l, left_page);
	if (right_page < total_pages) {
		write_page_to_epd(epd_r, right_page);
	}

	sd_power_off();

	/* Trigger both refreshes — they run in parallel */
	display_blanking_off(epd_l);
	if (right_page < total_pages) {
		display_blanking_off(epd_r);
	}
}

static int count_pages(void)
{
	int count = 0;

	while (page_exists(count)) {
		count++;
	}
	return count;
}

int main(void)
{
	int total_spreads;

	k_sleep(K_MSEC(1000));
	printk("\n\n=== E-Reader boot ===\n");

	leds = DEVICE_DT_GET(DT_NODELABEL(pmic_leds));
	epd_l = DEVICE_DT_GET(DT_NODELABEL(epd_left));
	epd_r = DEVICE_DT_GET(DT_NODELABEL(epd_right));

	if (!device_is_ready(leds)) {
		leds = NULL;
	}

	if (!device_is_ready(epd_l)) {
		printk("EPD left display not ready\n");
		return -1;
	}

	if (!device_is_ready(epd_r)) {
		printk("EPD right display not ready\n");
		return -1;
	}

	sd_ldsw = DEVICE_DT_GET(DT_NODELABEL(pmic_ldsw1));
	if (!device_is_ready(sd_ldsw)) {
		printk("PMIC load switch not ready\n");
		return -1;
	}

	if (!sd_power_on()) {
		printk("SD card init failed\n");
		return -1;
	}

	total_pages = count_pages();
	printk("Found %d pages\n", total_pages);

	sd_power_off();

	if (total_pages == 0) {
		printk("No pages on SD card\n");
		return -1;
	}

	total_spreads = (total_pages + PAGES_PER_SPREAD - 1) / PAGES_PER_SPREAD;
	current_spread = 0;

	show_spread();

	while (1) {
		k_sem_take(&button_sem, K_FOREVER);

		if (leds) {
			led_on(leds, 0);
		}

		switch (pending_action) {
		case BTN_NEXT:
			if (current_spread + 1 < total_spreads) {
				current_spread++;
				show_spread();
			}
			break;
		case BTN_PREV:
			if (current_spread > 0) {
				current_spread--;
				show_spread();
			}
			break;
		default:
			break;
		}

		pending_action = BTN_NONE;

		if (leds) {
			led_off(leds, 0);
		}
	}
}
