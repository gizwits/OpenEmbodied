#!/bin/bash

#==============================================
# 使用说明:
#----------------------------------------------
# ./1build.sh          # 常规重新编译
# ./1build.sh goon     # 继续上次编译
# ./1build.sh r        # 彻底删除build目录
# ./1build.sh c        # 做一次fullclean
#==============================================

clear 

# 不使用lib编译打开下行代码, 切换后做一次./1build.sh r
unset USE_SDK_LIB

# 使用lib编译打开下行代码, 切换后做一次./1build.sh r
# export USE_SDK_LIB=1

if [ -n "$1" ] ; then

    if [  "$1" != "goon" ] ; then
        echo "continue build!"
        . export_adf.sh
    fi

    if [ "$1" = "r" ]; then
        echo "remove build!"
        rm -rf build
        # idf.py set-target esp32s3
    elif [ "$1" = "c" ]; then
        echo "clean!"
        idf.py fullclean
        exit
    fi

    idf.py build

else
. export_adf.sh
idf.py build
fi

./2flash.sh