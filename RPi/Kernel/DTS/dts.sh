#!/bin/bash

# === 設定區 ===
ADC_DTS_NAME="ads1115-ws2812b-overlay"
ADC_DTS_FILE="${ADC_DTS_NAME}.dts"
ADC_DTBO_FILE="${ADC_DTS_NAME}.dtbo"
LED_DTS_NAME="meme-ws2812"
LED_DTS_FILE="${LED_DTS_NAME}.dts"
LED_DTBO_FILE="${LED_DTS_NAME}.dtbo"
OVERLAYS_DIR="/boot/overlays"
CONFIG_FILE="/boot/config.txt"
ENABLE_REBOOT=true  # 若不想自動重啟，設為 false

# === 檢查檔案是否存在 ===
if [ ! -f "$ADC_DTS_FILE" ]; then
    echo "DTS 檔案不存在：$ADC_DTS_FILE"
    exit 1
fi

# === 編譯 DTS ===
echo "正在編譯 $ADC_DTS_FILE ..."
dtc -@ -I dts -O dtb -o "$ADC_DTBO_FILE" "$ADC_DTS_FILE"
if [ $? -ne 0 ]; then
    echo "DTS $ADC_DTS_FILE 編譯失敗！ "
    exit 2
fi

echo "正在編譯 $LED_DTS_FILE ..."
dtc -@ -I dts -O dtb -o "$LED_DTBO_FILE" "$LED_DTS_FILE"
if [ $? -ne 0 ]; then
    echo "DTS $LED_DTS_FILE 編譯失敗！"
    exit 3
fi

# === 複製到 /boot/overlays/ ===
echo "複製 $ADC_DTBO_FILE → $OVERLAYS_DIR ..."
sudo cp "$ADC_DTBO_FILE" "$OVERLAYS_DIR/"
if [ $? -ne 0 ]; then
    echo "複製失敗，請確認有 sudo 權限"
    exit 4
fi

echo "複製 $LED_DTBO_FILE → $OVERLAYS_DIR ..."
sudo cp "$LED_DTBO_FILE" "$OVERLAYS_DIR/"
if [ $? -ne 0 ]; then
    echo "複製失敗，請確認有 sudo 權限"
    exit 5
fi

# === 檢查並寫入 /boot/config.txt ===
if ! grep -q "dtoverlay=$ADC_DTS_NAME" "$CONFIG_FILE"; then
    echo "寫入 dtoverlay=$ADC_DTS_NAME 到 $CONFIG_FILE"
    echo "dtoverlay=$ADC_DTS_NAME" | sudo tee -a "$CONFIG_FILE"
else
    echo "config.txt 已包含 dtoverlay=$ADC_DTS_NAME，略過修改"
fi


if ! grep -q "dtoverlay=$LED_DTS_NAME" "$CONFIG_FILE"; then
    echo "寫入 dtoverlay=$LED_DTS_NAME 到 $CONFIG_FILE"
    echo "dtoverlay=$LED_DTS_NAME" | sudo tee -a "$CONFIG_FILE"
else
    echo "config.txt 已包含 dtoverlay=$LED_DTS_NAME，略過修改"
fi

# === 完成提示 ===
echo "Overlay 編譯與設定完成"

# === 選擇性重新開機 ===
if [ "$ENABLE_REBOOT" = true ]; then
    echo "將在 5 秒後重新啟動 Raspberry Pi 套用 overlay ..."
    sleep 5
    sudo reboot
else
    echo "請手動執行 reboot 套用 overlay"
fi
