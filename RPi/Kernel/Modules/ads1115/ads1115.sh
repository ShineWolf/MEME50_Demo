#!/bin/bash

DTS_NAME="ads1115-ws2812b-overlay"
WS2812_DIR="../ws2812"
WS2812_NAME="meme-ws2812"
ADS1115_DIR="./ads1115/"
ADS1115_NAME="ads1115_overlay"
TARGET_DIR="/lib/modules/$(uname -r)/kernel/ads1115/"

cd $ADS1115_DIR
cp $WS2812_DIR/$WS2812_NAME.c ./

if [ ! -d "$TARGET_DIR" ]; then
    echo "目標目錄不存在：$DIR，自動建立該目錄"
    sudo mkdir $TARGET_DIR
fi

make clean
make
cp ./$ADS1115_NAME.ko $TARGET_DIR
cp ./$WS2812_NAME.ko $TARGET_DIR
depmod -ae

rm ./$WS2812_NAME*
