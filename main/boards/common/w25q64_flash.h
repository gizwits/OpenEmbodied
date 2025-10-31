#pragma once

#include <esp_err.h>
#include <driver/spi_master.h>
#include <esp_flash.h>
#include <stdint.h>
#include <stdbool.h>

// W25Q64 命令定义
#define W25Q64_CMD_WRITE_ENABLE       0x06
#define W25Q64_CMD_WRITE_DISABLE      0x04
#define W25Q64_CMD_READ_STATUS_REG1   0x05
#define W25Q64_CMD_READ_STATUS_REG2   0x35
#define W25Q64_CMD_WRITE_STATUS_REG   0x01
#define W25Q64_CMD_PAGE_PROGRAM       0x02
#define W25Q64_CMD_QUAD_PAGE_PROGRAM  0x32
#define W25Q64_CMD_BLOCK_ERASE_64K    0xD8
#define W25Q64_CMD_BLOCK_ERASE_32K    0x52
#define W25Q64_CMD_SECTOR_ERASE_4K    0x20
#define W25Q64_CMD_CHIP_ERASE         0xC7
#define W25Q64_CMD_ERASE_SUSPEND      0x75
#define W25Q64_CMD_ERASE_RESUME       0x7A
#define W25Q64_CMD_POWER_DOWN         0xB9
#define W25Q64_CMD_READ_DATA          0x03
#define W25Q64_CMD_FAST_READ          0x0B
#define W25Q64_CMD_READ_JEDEC_ID      0x9F
#define W25Q64_CMD_READ_UNIQUE_ID     0x4B
#define W25Q64_CMD_RELEASE_POWER_DOWN 0xAB

// W25Q64/W25Q128 参数定义
#define W25Q64_PAGE_SIZE         256        // 页大小 256 字节
#define W25Q64_SECTOR_SIZE       4096       // 扇区大小 4KB
#define W25Q64_BLOCK_32K_SIZE    32768      // 32KB 块大小
#define W25Q64_BLOCK_64K_SIZE    65536      // 64KB 块大小
#define W25Q64_CHIP_SIZE         (8*1024*1024)  // W25Q64 芯片大小 8MB
#define W25Q128_CHIP_SIZE        (16*1024*1024) // W25Q128 芯片大小 16MB
#define W25Q64_JEDEC_ID          0xEF4017   // W25Q64 的 JEDEC ID
#define W25Q128_JEDEC_ID         0xEF4018   // W25Q128 的 JEDEC ID

// 状态寄存器位定义
#define W25Q64_SR_BUSY           0x01       // 忙标志位
#define W25Q64_SR_WEL            0x02       // 写使能标志位

class W25Q64Flash {
public:
    // 获取单例实例
    static W25Q64Flash& GetInstance();
    
    // 删除拷贝构造和赋值操作符
    W25Q64Flash(const W25Q64Flash&) = delete;
    W25Q64Flash& operator=(const W25Q64Flash&) = delete;

    // 初始化 Flash
    esp_err_t Initialize(int mosi_pin, int miso_pin, int clk_pin, int cs_pin, int spi_freq_khz = 10000);
    
    // 反初始化
    void Deinitialize();

    // 基础操作
    esp_err_t ReadJedecId(uint32_t* jedec_id);
    esp_err_t ReadUniqueId(uint8_t* unique_id);
    esp_err_t ReadStatusRegister(uint8_t* status);
    
    // 读操作
    esp_err_t Read(uint32_t address, uint8_t* data, size_t length);
    esp_err_t FastRead(uint32_t address, uint8_t* data, size_t length);
    
    // 写操作（需要先擦除）
    esp_err_t PageProgram(uint32_t address, const uint8_t* data, size_t length);
    esp_err_t Write(uint32_t address, const uint8_t* data, size_t length);
    
    // 擦除操作
    esp_err_t SectorErase(uint32_t address);      // 擦除 4KB 扇区
    esp_err_t Block32KErase(uint32_t address);    // 擦除 32KB 块
    esp_err_t Block64KErase(uint32_t address);    // 擦除 64KB 块
    esp_err_t ChipErase();                        // 擦除整个芯片
    
    // 电源管理
    esp_err_t PowerDown();
    esp_err_t ReleasePowerDown();
    
    // 自检测试
    bool SelfTest();
    
    // 获取 Flash 信息
    uint32_t GetCapacity() const { return chip_size_; }
    uint32_t GetPageSize() const { return W25Q64_PAGE_SIZE; }
    uint32_t GetSectorSize() const { return W25Q64_SECTOR_SIZE; }
    bool IsInitialized() const { return initialized_; }
    
private:
    W25Q64Flash();  // 构造函数私有化
    ~W25Q64Flash(); // 析构函数私有化
    
    spi_device_handle_t spi_handle_;
    esp_flash_t* esp_flash_handle_;
    bool initialized_;
    int cs_pin_;
    uint32_t chip_size_;
    uint32_t jedec_id_;
    
    // 内部辅助函数
    esp_err_t WriteEnable();
    esp_err_t WriteDisable();
    esp_err_t WaitBusy(uint32_t timeout_ms = 5000);
    esp_err_t SendCommand(uint8_t cmd);
    esp_err_t SendCommandWithAddress(uint8_t cmd, uint32_t address);
};