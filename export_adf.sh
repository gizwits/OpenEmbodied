#!/bin/bash

if [ "$USER" == "martin" ]; then
    IDF_VERSION="v5.3.2"

    # export ADF_PATH="$HOME/esp32/esp-adf-newest"
    export ADF_PATH="$HOME/esp32/esp-adf-gizwits"
    # export ADF_PATH="$HOME/giz_adf/esp-adf"
    # export IDF_PATH="$HOME/esp32/esp-idf-$IDF_VERSION"
    export IDF_PATH="$HOME/esp32/esp-adf-gizwits/esp-idf"
    ESP_INSTALL_DIR="$HOME/.espressif-$IDF_VERSION"

    export PATH=${PATH}:${IDF_PATH}/tools/

    # 创建指向esp-idf软链接
    ESP_DIR="$HOME/.espressif"

    if [ -L "$ESP_DIR" ]; then
        echo "Delete link to ~/.espressif"
        rm "$ESP_DIR"
    fi

    if [ ! -e "$ESP_DIR" ]; then
        echo "Recreate $ESP_DIR link to $ESP_INSTALL_DIR"
        ln -s "$ESP_INSTALL_DIR" "$ESP_DIR"
    fi
elif [ "$USER" == "root" ]; then
    export ADF_PATH="$HOME/giz_adf/esp-adf"
    export IDF_PATH="$ADF_PATH/esp-idf"
fi


# call '. export_idf.sh' in bash
. "${ADF_PATH}/export.sh"
