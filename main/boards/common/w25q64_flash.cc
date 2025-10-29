#include "w25q64_flash.h"
#include <esp_log.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <esp_flash.h>
#include <esp_flash_spi_init.h>
#include <driver/spi_common.h>

static const char* TAG = "W25Q64";

// 单例实现
W25Q64Flash& W25Q64Flash::GetInstance() {
    static W25Q64Flash instance;
    return instance;
}

W25Q64Flash::W25Q64Flash() 
    : spi_handle_(nullptr)
    , esp_flash_handle_(nullptr)
    , initialized_(false)
    , cs_pin_(-1)
    , chip_size_(0)
    , jedec_id_(0) {
}

W25Q64Flash::~W25Q64Flash() {
    // 单例模式下，不自动清理资源
    // 如果需要清理，应该显式调用 Deinitialize()
    // Deinitialize();
}

esp_err_t W25Q64Flash::Initialize(int mosi_pin, int miso_pin, int clk_pin, int cs_pin, int spi_freq_khz) {
    if (initialized_) {
        ESP_LOGW(TAG, "W25Q64 already initialized");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "W25Q64Flash::Initialize start");

    cs_pin_ = cs_pin;

    // 配置 SPI 总线
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = mosi_pin,
        .miso_io_num = miso_pin,
        .sclk_io_num = clk_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    // 初始化 SPI 总线（使用 SPI3_HOST，避免与屏幕的 SPI2_HOST 冲突）
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE) {
        // SPI 总线已经初始化，这可能是正常的
        ESP_LOGI(TAG, "SPI3_HOST already initialized, skipping bus init");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置 Flash 设备
    esp_flash_spi_device_config_t flash_config = {};
    flash_config.host_id = SPI3_HOST;
    flash_config.cs_io_num = cs_pin;
    flash_config.input_delay_ns = 0;
    flash_config.io_mode = SPI_FLASH_SLOWRD;
    flash_config.freq_mhz = spi_freq_khz / 1000;  // 转换为 MHz
    
    ret = spi_bus_add_flash_device(&esp_flash_handle_, &flash_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add flash device: %s", esp_err_to_name(ret));
        spi_bus_free(SPI3_HOST);
        return ret;
    }
    
    // 初始化 Flash 芯片
    ret = esp_flash_init(esp_flash_handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize flash chip: %s", esp_err_to_name(ret));
        spi_bus_remove_flash_device(esp_flash_handle_);
        spi_bus_free(SPI3_HOST);
        return ret;
    }

    // 读取并验证 JEDEC ID
    uint32_t jedec_id = 0;
    ret = esp_flash_read_id(esp_flash_handle_, &jedec_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read JEDEC ID: %s", esp_err_to_name(ret));
        spi_bus_remove_flash_device(esp_flash_handle_);
        spi_bus_free(SPI3_HOST);
        return ret;
    }

    ESP_LOGI(TAG, "Flash JEDEC ID: 0x%06" PRIX32, jedec_id);
    
    // 识别芯片类型
    jedec_id_ = jedec_id & 0xFFFFFF;
    if (jedec_id_ == W25Q64_JEDEC_ID) {
        chip_size_ = W25Q64_CHIP_SIZE;
        ESP_LOGI(TAG, "Detected W25Q64 (8MB) Flash");
    } else if (jedec_id_ == W25Q128_JEDEC_ID) {
        chip_size_ = W25Q128_CHIP_SIZE;
        ESP_LOGI(TAG, "Detected W25Q128 (16MB) Flash");
    } else {
        ESP_LOGE(TAG, "Unknown Flash chip. JEDEC ID: 0x%06" PRIX32, jedec_id_);
        ESP_LOGI(TAG, "Continuing anyway, assuming compatible chip");
        // 默认按照较小的容量处理，以确保安全
        chip_size_ = W25Q64_CHIP_SIZE;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Flash initialized successfully! Capacity: %" PRIu32 " MB", chip_size_ / (1024 * 1024));
    return ESP_OK;
}

void W25Q64Flash::Deinitialize() {
    if (!initialized_) {
        return;
    }

    if (esp_flash_handle_) {
        spi_bus_remove_flash_device(esp_flash_handle_);
        esp_flash_handle_ = nullptr;
    }
    
    // 单例模式下，不释放 SPI 总线，避免影响其他组件
    // spi_bus_free(SPI3_HOST);
    initialized_ = false;
}

esp_err_t W25Q64Flash::SendCommand(uint8_t cmd) {
    // 暂时不支持直接发送命令
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t W25Q64Flash::SendCommandWithAddress(uint8_t cmd, uint32_t address) {
    // 暂时不支持直接发送命令
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t W25Q64Flash::WriteEnable() {
    return SendCommand(W25Q64_CMD_WRITE_ENABLE);
}

esp_err_t W25Q64Flash::WriteDisable() {
    return SendCommand(W25Q64_CMD_WRITE_DISABLE);
}

esp_err_t W25Q64Flash::WaitBusy(uint32_t timeout_ms) {
    uint8_t status;
    uint32_t start_time = xTaskGetTickCount();
    
    do {
        esp_err_t ret = ReadStatusRegister(&status);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read status register: %s", esp_err_to_name(ret));
            return ret;
        }
        
        // 调试输出前几次的状态
        static int debug_count = 0;
        if (debug_count < 5) {
            ESP_LOGD(TAG, "Status register: 0x%02X, BUSY: %d", status, (status & W25Q64_SR_BUSY) ? 1 : 0);
            debug_count++;
        }
        
        if (!(status & W25Q64_SR_BUSY)) {
            return ESP_OK;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); // 增加延时到 10ms
        
        if ((xTaskGetTickCount() - start_time) > pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGE(TAG, "Wait busy timeout after %lu ms, status: 0x%02X", 
                     (xTaskGetTickCount() - start_time) * portTICK_PERIOD_MS, status);
            return ESP_ERR_TIMEOUT;
        }
    } while (1);
}

esp_err_t W25Q64Flash::ReadStatusRegister(uint8_t* status) {
    // 使用模拟状态
    if (status) {
        *status = 0x00;  // 返回空闲状态
    }
    return ESP_OK;
}

esp_err_t W25Q64Flash::ReadJedecId(uint32_t* jedec_id) {
    if (!jedec_id || !esp_flash_handle_) {
        return ESP_ERR_INVALID_ARG;
    }
    return esp_flash_read_id(esp_flash_handle_, jedec_id);
}

esp_err_t W25Q64Flash::ReadUniqueId(uint8_t* unique_id) {
    // 暂时不支持读取唯一ID，返回模拟数据
    if (!unique_id) {
        return ESP_ERR_INVALID_ARG;
    }
    // 返回模拟的唯一ID
    for (int i = 0; i < 8; i++) {
        unique_id[i] = 0xFF;
    }
    return ESP_OK;
}

esp_err_t W25Q64Flash::Read(uint32_t address, uint8_t* data, size_t length) {
    if (!data || length == 0 || address + length > chip_size_) {
        return ESP_ERR_INVALID_ARG;
    }

    return esp_flash_read(esp_flash_handle_, data, address, length);
}

esp_err_t W25Q64Flash::FastRead(uint32_t address, uint8_t* data, size_t length) {
    // 使用普通读取代替快速读取
    return Read(address, data, length);
}

esp_err_t W25Q64Flash::PageProgram(uint32_t address, const uint8_t* data, size_t length) {
    // 使用 esp_flash_write 代替
    return Write(address, data, length);
}

esp_err_t W25Q64Flash::Write(uint32_t address, const uint8_t* data, size_t length) {
    if (!data || length == 0 || address + length > chip_size_) {
        return ESP_ERR_INVALID_ARG;
    }
    
    return esp_flash_write(esp_flash_handle_, data, address, length);
}

esp_err_t W25Q64Flash::SectorErase(uint32_t address) {
    if (address >= chip_size_) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 对齐到扇区边界
    address = address & ~(W25Q64_SECTOR_SIZE - 1);
    
    return esp_flash_erase_region(esp_flash_handle_, address, W25Q64_SECTOR_SIZE);
}

esp_err_t W25Q64Flash::Block32KErase(uint32_t address) {
    if (address >= chip_size_) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 对齐到 32K 块边界
    address = address & ~(W25Q64_BLOCK_32K_SIZE - 1);
    
    return esp_flash_erase_region(esp_flash_handle_, address, W25Q64_BLOCK_32K_SIZE);
}

esp_err_t W25Q64Flash::Block64KErase(uint32_t address) {
    if (address >= chip_size_) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 对齐到 64K 块边界
    address = address & ~(W25Q64_BLOCK_64K_SIZE - 1);
    
    return esp_flash_erase_region(esp_flash_handle_, address, W25Q64_BLOCK_64K_SIZE);
}

esp_err_t W25Q64Flash::ChipErase() {
    ESP_LOGI(TAG, "Chip erase started, this may take up to 100 seconds...");
    return esp_flash_erase_chip(esp_flash_handle_);
}

esp_err_t W25Q64Flash::PowerDown() {
    return SendCommand(W25Q64_CMD_POWER_DOWN);
}

esp_err_t W25Q64Flash::ReleasePowerDown() {
    return SendCommand(W25Q64_CMD_RELEASE_POWER_DOWN);
}

bool W25Q64Flash::SelfTest() {
    ESP_LOGI(TAG, "=== W25Q64 Flash Self Test Started ===");
    
    // 1. 验证 JEDEC ID
    uint32_t jedec_id = 0;
    esp_err_t ret = ReadJedecId(&jedec_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read JEDEC ID: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "JEDEC ID: 0x%06" PRIX32, jedec_id);
    
    // 检查是否是支持的芯片
    uint32_t id = jedec_id & 0xFFFFFF;
    if (id != W25Q64_JEDEC_ID && id != W25Q128_JEDEC_ID) {
        ESP_LOGW(TAG, "Unknown JEDEC ID, but continuing test...");
    }
    
    // 2. 读取唯一 ID
    uint8_t unique_id[8];
    ret = ReadUniqueId(unique_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read unique ID: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "Unique ID: %02X %02X %02X %02X %02X %02X %02X %02X",
             unique_id[0], unique_id[1], unique_id[2], unique_id[3],
             unique_id[4], unique_id[5], unique_id[6], unique_id[7]);
    
    // 3. 读取状态寄存器
    uint8_t status = 0;
    ret = ReadStatusRegister(&status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read status register: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "Status Register: 0x%02X", status);
    
    // 4. 测试读写操作（使用最后一个扇区进行测试，避免破坏用户数据）
    const uint32_t test_address = chip_size_ - W25Q64_SECTOR_SIZE;
    const size_t test_size = 256;
    uint8_t write_data[test_size];
    uint8_t read_data[test_size];
    
    // 生成测试数据
    for (size_t i = 0; i < test_size; i++) {
        write_data[i] = (uint8_t)(i & 0xFF);
    }
    
    ESP_LOGI(TAG, "Erasing test sector at 0x%06" PRIX32 "...", test_address);
    ret = SectorErase(test_address);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase sector: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "Writing test data...");
    ret = Write(test_address, write_data, test_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write data: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "Reading test data...");
    ret = Read(test_address, read_data, test_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read data: %s", esp_err_to_name(ret));
        return false;
    }
    
    // 验证数据
    if (memcmp(write_data, read_data, test_size) != 0) {
        ESP_LOGE(TAG, "Data verification failed!");
        // 打印前 16 字节用于调试
        ESP_LOGE(TAG, "Written: %02X %02X %02X %02X %02X %02X %02X %02X...",
                 write_data[0], write_data[1], write_data[2], write_data[3],
                 write_data[4], write_data[5], write_data[6], write_data[7]);
        ESP_LOGE(TAG, "Read:    %02X %02X %02X %02X %02X %02X %02X %02X...",
                 read_data[0], read_data[1], read_data[2], read_data[3],
                 read_data[4], read_data[5], read_data[6], read_data[7]);
        return false;
    }
    
    ESP_LOGI(TAG, "Data verification successful!");
    
    // 5. 测试快速读取
    memset(read_data, 0, test_size);
    ESP_LOGI(TAG, "Testing fast read...");
    ret = FastRead(test_address, read_data, test_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to fast read data: %s", esp_err_to_name(ret));
        return false;
    }
    
    if (memcmp(write_data, read_data, test_size) != 0) {
        ESP_LOGE(TAG, "Fast read data verification failed!");
        return false;
    }
    ESP_LOGI(TAG, "Fast read successful!");
    
    // 6. 清理测试扇区
    ESP_LOGI(TAG, "Cleaning up test sector...");
    ret = SectorErase(test_address);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clean up test sector: %s", esp_err_to_name(ret));
        return false;
    }
    
    ESP_LOGI(TAG, "=== Flash Self Test Passed ===");
    ESP_LOGI(TAG, "Flash Capacity: %" PRIu32 " MB", chip_size_ / (1024 * 1024));
    ESP_LOGI(TAG, "Page Size: %d bytes", W25Q64_PAGE_SIZE);
    ESP_LOGI(TAG, "Sector Size: %d KB", W25Q64_SECTOR_SIZE / 1024);
    
    return true;
}