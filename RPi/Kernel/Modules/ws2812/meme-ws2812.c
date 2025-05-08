#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#define DRIVER_NAME "meme-ws2812"
#define DEVICE_NAME "ws2812"

#define WS2812_BITS_PER_BYTE 8
#define WS2812_BITS_PER_COLOR (8 * 3)  // 每個顏色 8bit，每 bit 模擬 3bit
#define WS2812_BITS_PER_LED (3 * WS2812_BITS_PER_COLOR) // G R B 各一組
#define WS2812_SPI_BYTES_PER_LED (WS2812_BITS_PER_LED / 8) // = 72 bits / 8 = 9 bytes

static struct spi_device *ws2812_spi;

void ws2812_send_from_kernel(const u8 *rgb, int count);

/* 模擬每個 byte（8bit），每 bit 轉為 3bit 模擬值 (110 或 100) , 共展開24bit*/
static void ws2812_encode_byte(u8 byte, u8 *out)
{
    for (int i = 7; i >= 0; i--) {
        if (byte & (1 << i))
            *out++ = 0b110;  // 邏輯 1
        else
            *out++ = 0b100;  // 邏輯 0
    }
}

/* 將 bit array（每 bit 是 3bits 模擬）打包為 SPI 傳送用的 8-bit 格式 */
static void ws2812_pack_bits(const u8 *in, int in_len, u8 *out)
{
    int bit_pos = 0;
    memset(out, 0, (in_len * 3 + 7) / 8);

    for (int i = 0; i < in_len; i++) {
        for (int j = 2; j >= 0; j--) {
            int bit = (in[i] >> j) & 1;
            int byte_pos = bit_pos / 8;
            int bit_offset = 7 - (bit_pos % 8);
            if (bit)
                out[byte_pos] |= (1 << bit_offset);
            bit_pos++;
        }
    }
}

void ws2812_send_from_kernel(const u8 *rgb, int count)
{
    int bits_len = count * 24;
    u8 *tmp_bits = kzalloc(bits_len, GFP_KERNEL);
    u8 *spi_buf = kzalloc(count * WS2812_SPI_BYTES_PER_LED, GFP_KERNEL);
    u8 *p = tmp_bits;
    
    if (!tmp_bits || !spi_buf) {
        pr_err("ws2812: memory alloc failed\n");
        goto cleanup;
    }

    for (int i = 0; i < count; i++) {
        ws2812_encode_byte(rgb[i * 3 + 1], p); p += 8; // G
        ws2812_encode_byte(rgb[i * 3 + 0], p); p += 8; // R
        ws2812_encode_byte(rgb[i * 3 + 2], p); p += 8; // B
    }

    ws2812_pack_bits(tmp_bits, bits_len, spi_buf);

    struct spi_transfer t = {
        .tx_buf = spi_buf,
        .len = count * WS2812_SPI_BYTES_PER_LED,
        .cs_change = 0,
    };
    struct spi_message m;
    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    spi_sync(ws2812_spi, &m);
cleanup:
    kfree(tmp_bits);
    kfree(spi_buf);
}
EXPORT_SYMBOL(ws2812_send_from_kernel);

static ssize_t ws2812_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    if (count % 3 != 0)
        return -EINVAL;

    u8 *rgb = kmalloc(count, GFP_KERNEL);
    if (!rgb)
        return -ENOMEM;

    if (copy_from_user(rgb, buf, count)) {
        kfree(rgb);
        return -EFAULT;
    }

    pr_info("ws2812: write %zu bytes (%zu LEDs)\n", count, count / 3);
    ws2812_send_from_kernel(rgb, count / 3);
    kfree(rgb);
    return count;
}

static const struct file_operations ws2812_fops = {
    .owner = THIS_MODULE,
    .write = ws2812_write,
};

static struct miscdevice ws2812_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEVICE_NAME,
    .fops = &ws2812_fops,
    .mode = 0666,
};

static const struct spi_device_id ws2812_id[] = {
    { "ws2812", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, ws2812_id);

static const struct of_device_id ws2812_dt_ids[] = {
    { .compatible = "meme,ws2812" },
    { }
};
MODULE_DEVICE_TABLE(of, ws2812_dt_ids);

static int ws2812_probe(struct spi_device *spi)
{
    ws2812_spi = spi;
    spi->mode = SPI_MODE_0;
    spi->max_speed_hz = 2400000;
    spi_setup(spi);

    misc_register(&ws2812_misc);
    pr_info("ws2812: /dev/ws2812 created\n");
    return 0;
}

static void ws2812_remove(struct spi_device *spi)
{
    struct spi_message m;
    u8 reset_buf[24] = { 0 };
    struct spi_transfer reset_t = {
        .tx_buf = reset_buf,
        .len = sizeof(reset_buf),
        .cs_change = 0,
    };
    spi_message_init(&m);
    spi_message_add_tail(&reset_t, &m);
    spi_sync(ws2812_spi, &m);
    misc_deregister(&ws2812_misc);
}

static struct spi_driver ws2812_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = ws2812_dt_ids,
    },
    .probe = ws2812_probe,
    .remove = ws2812_remove,
    .id_table = ws2812_id,
};
module_spi_driver(ws2812_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("meme");
MODULE_DESCRIPTION("SPI WS2812 driver with kernel-callable interface");
