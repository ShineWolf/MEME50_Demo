    #include <linux/module.h>
    #include <linux/kernel.h>
    #include <linux/init.h>
    #include <linux/i2c.h>
    #include <linux/kthread.h>
    #include <linux/delay.h>
    #include <linux/byteorder/generic.h>
    #include <linux/miscdevice.h>
    #include <linux/poll.h>

    #include <linux/of_gpio.h>
    #include <linux/platform_device.h>

    #define DRIVER_NAME "ads1115-ws2812"
    #define DEVICE_NAME "ads1115"
    #define DEVICE_ALERT_NAME "ads1115-alert"
    #define I2C_ADDR 0x48
    #define LED_COUNT 8
    #define ALERT_LEVEL 4

    static struct i2c_client *ads1115_client;
    static struct task_struct *poll_thread;
    static struct task_struct *led_thread;
    static DEFINE_MUTEX(ads1115_lock);
    static int latest_val = 0; // 給 user 讀取的最新值
    static int alert_val = 0; // 給 user 讀取的警告值
    static unsigned char *led_buf = NULL; //給 LED 的顏色
    static s32 sound_val = 0;
    static int sound_level = 0;
    static s32 base_line = 0;
    static s32 max_line = 32767;
    static DECLARE_WAIT_QUEUE_HEAD(ads1115_alert_wq);

    // 使用 A0 channel，MUX = A0-GND，FSR = +/-4.096V（PGA bits 001）
    #define ADS1115_CONFIG 0x01
    #define ADS1115_CONVERSION 0x00
    #define CONFIG_OS_SINGLE (1 << 15)
    #define CONFIG_MUX_AIN0 (4 << 12)
    #define CONFIG_PGA_4_096V (1 << 9)
    #define CONFIG_PGA_2_048V (2 << 9) 
    #define CONFIG_MODE_SINGLE (1 << 8)
    #define CONFIG_DR_128SPS (4 << 5)
    #define CONFIG_DEFAULT (CONFIG_OS_SINGLE | CONFIG_MUX_AIN0 | CONFIG_PGA_2_048V | CONFIG_MODE_SINGLE | CONFIG_DR_128SPS)

    extern void ws2812_send_from_kernel(const u8 *rgb, size_t count);

    static int ads1115_poll_fn(void *data) {
        int i;
        u8 config_buf[2];
        s32 tmp_val;
        
        if (!led_buf) {
            pr_err(DRIVER_NAME ": led_buf not initialized!\n");
            return -ENOMEM;
        }
        
        mutex_lock(&ads1115_lock);
        //初始化背景數值，以100次為限
        for(i = 0; i < 100; i++){
            // 每次都觸發一次單次轉換
            config_buf[0] = (CONFIG_DEFAULT >> 8) & 0xFF;
            config_buf[1] = CONFIG_DEFAULT & 0xFF;
            i2c_smbus_write_i2c_block_data(ads1115_client, ADS1115_CONFIG, 2, config_buf);
            msleep(15);
            
            tmp_val = i2c_smbus_read_word_data(ads1115_client, ADS1115_CONVERSION);
            tmp_val = (tmp_val >> 8) | ((tmp_val & 0xFF) << 8);
            if (tmp_val <= 0) {
                pr_err(DRIVER_NAME ": read error\n");
                i--;
            } else {
                base_line += tmp_val;
            }
            msleep(15);
        }
        //取得基準值
        base_line = base_line / 100;
        //取得最小範圍值
        max_line = (max_line - base_line > base_line) ? base_line : max_line - base_line;
        max_line = (max_line >> 3); // max_line * 0.125 縮小閾值
        mutex_unlock(&ads1115_lock);

        while (!kthread_should_stop()) {
            // 每次都觸發一次單次轉換
            config_buf[0] = (CONFIG_DEFAULT >> 8) & 0xFF;
            config_buf[1] = CONFIG_DEFAULT & 0xFF;
            i2c_smbus_write_i2c_block_data(ads1115_client, ADS1115_CONFIG, 2, config_buf);

            msleep(15); // 等待 conversion 完成

            mutex_lock(&ads1115_lock);
            sound_val = i2c_smbus_read_word_data(ads1115_client, ADS1115_CONVERSION);
            // swap bytes (ADS1115是big-endian)
            sound_val = (sound_val >> 8) | ((sound_val & 0xFF) << 8);
            mutex_unlock(&ads1115_lock);

            if (sound_val < 0) {
                pr_err(DRIVER_NAME ": read error\n");
            } else {
                mutex_lock(&ads1115_lock);
                latest_val = sound_val; 
                mutex_unlock(&ads1115_lock);
            }
            msleep(100);
        }
        return 0;
    }

    static int led_thread_fn(void *data) {
        s32 diff_val;
        s32 last_val = 0;
        int continue_flag = 0;
        int last_led_count = 1;
        unsigned char rgb1[] = {
            0xFF, 0x00, 0x00,  // LED 1: Red
            0x00, 0xFF, 0x00,  // LED 2: Green
            0x00, 0x00, 0xFF,  // LED 3: Blue
            0xFF, 0xFF, 0xFF,  // LED 4: White
            0x00, 0x00, 0x00,  // LED 5: None
            0x00, 0x00, 0x00,  // LED 6: None
            0x00, 0x00, 0x00,  // LED 7: None
            0x00, 0x00, 0x00   // LED 8: None
        };

        unsigned char rgb2[] = {
            0x00, 0x00, 0x00,  // LED 1: None
            0x00, 0x00, 0x00,  // LED 2: None
            0x00, 0x00, 0x00,  // LED 3: None
            0x00, 0x00, 0x00,   // LED 4: None
            0xFF, 0x00, 0x00,  // LED 5: Red
            0x00, 0xFF, 0x00,  // LED 6: Green
            0x00, 0x00, 0xFF,  // LED 7: Blue
            0xFF, 0xFF, 0xFF   // LED 8: White
        };

        unsigned char reset[] = {
            0x00, 0x00, 0x00,  // LED 1: None
            0x00, 0x00, 0x00,  // LED 2: None
            0x00, 0x00, 0x00,  // LED 3: None
            0x00, 0x00, 0x00,  // LED 4: None
            0x00, 0x00, 0x00,  // LED 5: None
            0x00, 0x00, 0x00,  // LED 6: None
            0x00, 0x00, 0x00,  // LED 7: None
            0x00, 0x00, 0x00   // LED 8: None
        };


        // 寫入 LED
        ws2812_send_from_kernel(rgb1, LED_COUNT * 3);
        msleep(3000);
        ws2812_send_from_kernel(reset, LED_COUNT * 3);
        msleep(3000);
        ws2812_send_from_kernel(rgb2, LED_COUNT * 3);
        msleep(3000);
        ws2812_send_from_kernel(reset, LED_COUNT * 3);
        msleep(3000);


        while (!kthread_should_stop()) {
            usleep_range(290, 350);
            // 轉成絕對值（以基準點為準
            mutex_lock(&ads1115_lock);
            if (last_val == sound_val){
                continue_flag = 1;
            }
            last_val = sound_val;
            mutex_unlock(&ads1115_lock);
            if (continue_flag){
                continue_flag = 0;
                msleep(200);
                continue;
            }
            mutex_lock(&ads1115_lock);
            diff_val = (sound_val > base_line)? sound_val - base_line : base_line - sound_val;

            // 把範圍壓到1~8顆燈
            sound_level = diff_val * LED_COUNT / max_line;
            if (sound_level > 7) 
                sound_level = 7;
            //讓燈至少維持一盞燈，不讓他閃爍
            sound_level = (sound_level == 0) ? last_led_count : sound_level;
            last_led_count = sound_level;
            
            if (sound_level > ALERT_LEVEL){
                alert_val = sound_val;
                wake_up_interruptible(&ads1115_alert_wq);
            }

            // 因應前台顯示要求，希望數字越大表示大聲，越小表示小聲
            sound_val = base_line + diff_val;
            
            mutex_unlock(&ads1115_lock);
            pr_info("[WS2812] 音量: %d default: %d max_line:%d → 顯示 %d 顆燈\n", sound_val, base_line, max_line, sound_level + 1);
            // 計算顯示燈數（sound_level）
            for (int i = 0; i < LED_COUNT; i++) {
                if (i < sound_level + 1) {
                    if (i < ALERT_LEVEL){
                        led_buf[i * 3 + 0] = 0x00; // R
                        led_buf[i * 3 + 1] = 0xFF; // G
                        led_buf[i * 3 + 2] = 0x00; // B
                    } else {
                        led_buf[i * 3 + 0] = 0xFF; // R
                        led_buf[i * 3 + 1] = 0x00; // G
                        led_buf[i * 3 + 2] = 0x00; // B
                    }
                } else {
                    led_buf[i * 3 + 0] = 0x00;
                    led_buf[i * 3 + 1] = 0x00;
                    led_buf[i * 3 + 2] = 0x00;
                }
            }

            // 寫入 LED
            ws2812_send_from_kernel(led_buf, LED_COUNT * 3);
        }
        return 0;
    }

    static ssize_t ads1115_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
    {
        char kbuf[16];
        int len;

        mutex_lock(&ads1115_lock);
        len = snprintf(kbuf, sizeof(kbuf), "%d\n", latest_val);
        mutex_unlock(&ads1115_lock);

        if (copy_to_user(buf, kbuf, len))
            return -EFAULT;

        return len;
    }

    static unsigned int ads1115_poll(struct file *file, poll_table *wait)
    {
        // 這邊暫時不實作 wait queue，先回固定值表示「always ready」
        return POLLIN | POLLRDNORM;
    }

    static const struct file_operations ads1115_fops = {
        .owner = THIS_MODULE,
        .read = ads1115_read,
        .poll = ads1115_poll,
    };

    static struct miscdevice ads1115_misc = {
        .minor = MISC_DYNAMIC_MINOR,
        .name = DEVICE_NAME,
        .fops = &ads1115_fops,
        .mode = 0666,
    };

    static ssize_t ads1115_read_alert(struct file *file, char __user *buf, size_t count, loff_t *ppos)
    {
        char kbuf[16];
        int len;

        mutex_lock(&ads1115_lock);
        len = snprintf(kbuf, sizeof(kbuf), "%d\n", alert_val);
        mutex_unlock(&ads1115_lock);

        if (copy_to_user(buf, kbuf, len))
            return -EFAULT;

        return len;
    }

    static loff_t ads1115_llseek(struct file *file, loff_t offset, int whence)
    {
        // 讓 lseek() 成功，把 offset 重設成 0
        file->f_pos = 0;
        return 0;
    }

    static ssize_t ads1115_write_alert(struct file *filp, const char __user *buf, size_t len, loff_t *off) {
        char kbuf[16];

        if (len >= sizeof(kbuf))
            return -EINVAL;  // 避免過長輸入

        if (copy_from_user(kbuf, buf, len))
            return -EFAULT;

        // 檢查是否為 "clear\n" 或 "clear"
        if (strncmp(kbuf, "clear", 5) == 0) {
            mutex_lock(&ads1115_lock);
            alert_val = 0;
            wake_up_interruptible(&ads1115_alert_wq);
            mutex_unlock(&ads1115_lock);
            pr_info(DRIVER_NAME ": alert_val cleared\n");
            return len;
        }

        return -EINVAL;  // 不接受其他指令
    }

    static unsigned int ads1115_poll_alert(struct file *filp, struct poll_table_struct *wait)
    {
        unsigned int mask = 0;

        // 將目前 process 加入 wait queue
        poll_wait(filp, &ads1115_alert_wq, wait);

        mutex_lock(&ads1115_lock);
        if (alert_val > 0) {
            mask |= POLLIN | POLLRDNORM;  // 有資料可讀
        }
        mutex_unlock(&ads1115_lock);

        return mask;
    }

    static const struct file_operations ads1115_alert_fops = {
        .owner = THIS_MODULE,
        .read = ads1115_read_alert,
        .write = ads1115_write_alert,
        .poll = ads1115_poll_alert,
        .llseek = ads1115_llseek,
    };

    static struct miscdevice ads1115_alert_misc = {
        .minor = MISC_DYNAMIC_MINOR,
        .name = DEVICE_ALERT_NAME,
        .fops = &ads1115_alert_fops,
        .mode = 0666,
    };

    static const struct of_device_id ads1115_ws2812_of_match[] = {
        { .compatible = "ads1115_ws2812", },
    };

    static int ads1115_ws2812_probe(struct platform_device *pdev)
    {
        struct device *dev = &pdev->dev;
        struct device_node *np = dev->of_node;
        struct device_node *i2c_np;
        struct i2c_client *client;
        int ret;

        dev_info(dev, "ads1115_ws2812_probe called\n");

        // 不要自己 i2c_get_adapter / i2c_new_client_device！DTS已經裝好了

        led_buf = kzalloc(LED_COUNT * 3, GFP_KERNEL);
        if (!led_buf){
            dev_err(dev, "Failed to allocate led_buf\n");
            return -ENOMEM;
        }

        // 找到i2c子節點
        i2c_np = of_parse_phandle(np, "i2c-parent", 0);
        if (!i2c_np) {
            dev_err(dev, "Failed to parse i2c-parent\n");
            return -ENODEV;
        }

        client = of_find_i2c_device_by_node(i2c_np);
        if (!client) {
            dev_err(dev, "Failed to find i2c client\n");
            return -EPROBE_DEFER;
        }

        // 儲存到 platform_data 或其他結構
        platform_set_drvdata(pdev, client);

        ads1115_client = client;

        poll_thread = kthread_run(ads1115_poll_fn, NULL, "ads1115_poll");
        if (IS_ERR(poll_thread)) {
            dev_err(dev, "Failed to create poll thread\n");
            return PTR_ERR(poll_thread);
        }

        led_thread = kthread_run(led_thread_fn, NULL, "ws2812_led_updater");
        if (IS_ERR(led_thread)) {
            dev_err(dev, "Failed to create ws2812_led_updater thread\n");
            return PTR_ERR(led_thread);
        }

        ret = misc_register(&ads1115_misc);
        if (ret) {
            kthread_stop(poll_thread);
            kthread_stop(led_thread);
            return ret;
        }
        ret = misc_register(&ads1115_alert_misc);
        if (ret) {
            misc_deregister(&ads1115_misc);
            kthread_stop(poll_thread);
            kthread_stop(led_thread);
            return ret;
        }

        dev_info(dev, "ads1115_ws2812 driver loaded successfully\n");
        return 0;
    }

    static void ads1115_ws2812_remove(struct platform_device *pdev) {
        if (poll_thread)
            kthread_stop(poll_thread);

        if (led_thread)
            kthread_stop(led_thread);

        //這邊不要移除ads1115_client，這樣重開程式後能繼續使用
        //i2c_unregister_device(ads1115_client);
        ads1115_client = NULL;

        if (led_buf) {
            kfree(led_buf);
            led_buf = NULL;
        }

        misc_deregister(&ads1115_misc);
        misc_deregister(&ads1115_alert_misc);
        
        pr_info(DRIVER_NAME ": module unloaded\n");
    }


    MODULE_DEVICE_TABLE(of, ads1115_ws2812_of_match);

    static struct platform_driver ads1115_ws2812_driver = {
        .driver = {
            .name = DRIVER_NAME,
            .of_match_table = ads1115_ws2812_of_match,
        },
        .probe = ads1115_ws2812_probe,
        .remove = ads1115_ws2812_remove
    };

    module_platform_driver(ads1115_ws2812_driver);

    MODULE_LICENSE("GPL");
    MODULE_AUTHOR("JayLiao");
    MODULE_DESCRIPTION("Sound monitor");
