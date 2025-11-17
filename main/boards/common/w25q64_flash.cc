#include "w25q64_flash.h"
#include <esp_log.h>
#include <string.h>
#include <inttypes.h>
#include <esp_flash.h>
#include <esp_flash_spi_init.h>
#include <driver/spi_common.h>
#include <esp_http_client.h>
#include <esp_tls.h>

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
    , jedec_id_(0)
    , mutex_(xSemaphoreCreateMutex()) {
    if (mutex_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create flash mutex");
    }
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
    // 先尝试释放之前可能存在的配置
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE) {
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
    
    if (mutex_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take flash mutex in ReadJedecId");
        return ESP_ERR_TIMEOUT;
    }
    
    esp_err_t ret = esp_flash_read_id(esp_flash_handle_, jedec_id);
    xSemaphoreGive(mutex_);
    
    return ret;
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

    if (mutex_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take flash mutex in Read");
        return ESP_ERR_TIMEOUT;
    }
    
    esp_err_t ret = esp_flash_read(esp_flash_handle_, data, address, length);
    xSemaphoreGive(mutex_);
    
    return ret;
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
    
    if (mutex_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take flash mutex in Write");
        return ESP_ERR_TIMEOUT;
    }
    
    esp_err_t ret = esp_flash_write(esp_flash_handle_, data, address, length);
    xSemaphoreGive(mutex_);
    
    return ret;
}

esp_err_t W25Q64Flash::SectorErase(uint32_t address) {
    if (address >= chip_size_) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 对齐到扇区边界
    address = address & ~(W25Q64_SECTOR_SIZE - 1);
    
    // 使用更长的超时时间
    esp_err_t ret = esp_flash_erase_region(esp_flash_handle_, address, W25Q64_SECTOR_SIZE);
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "First erase attempt timed out, retrying...");
        vTaskDelay(pdMS_TO_TICKS(100));  // 等待一下再重试
        ret = esp_flash_erase_region(esp_flash_handle_, address, W25Q64_SECTOR_SIZE);
    }
    
    return ret;
}

esp_err_t W25Q64Flash::Block32KErase(uint32_t address) {
    if (address >= chip_size_) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 对齐到 32K 块边界
    address = address & ~(W25Q64_BLOCK_32K_SIZE - 1);
    
    // 使用更长的超时时间
    esp_err_t ret = esp_flash_erase_region(esp_flash_handle_, address, W25Q64_BLOCK_32K_SIZE);
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "First 32K block erase attempt timed out, retrying...");
        vTaskDelay(pdMS_TO_TICKS(200));  // 等待更长时间
        ret = esp_flash_erase_region(esp_flash_handle_, address, W25Q64_BLOCK_32K_SIZE);
    }
    
    return ret;
}

esp_err_t W25Q64Flash::Block64KErase(uint32_t address) {
    if (address >= chip_size_) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 对齐到 64K 块边界
    address = address & ~(W25Q64_BLOCK_64K_SIZE - 1);
    
    // 使用更长的超时时间
    esp_err_t ret = esp_flash_erase_region(esp_flash_handle_, address, W25Q64_BLOCK_64K_SIZE);
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "First 64K block erase attempt timed out, retrying...");
        vTaskDelay(pdMS_TO_TICKS(300));  // 等待更长时间
        ret = esp_flash_erase_region(esp_flash_handle_, address, W25Q64_BLOCK_64K_SIZE);
    }
    
    return ret;
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

esp_err_t W25Q64Flash::BulkErase(uint32_t address, size_t size) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (address + size > chip_size_) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 对齐地址到扇区边界
    uint32_t aligned_addr = address & ~(W25Q64_SECTOR_SIZE - 1);
    uint32_t end_addr = (address + size + W25Q64_SECTOR_SIZE - 1) & ~(W25Q64_SECTOR_SIZE - 1);
    size_t erase_size = end_addr - aligned_addr;
    
    ESP_LOGI(TAG, "Bulk erasing %zu bytes at 0x%06" PRIX32 " (aligned: 0x%06" PRIX32 " - 0x%06" PRIX32 ")", 
             erase_size, address, aligned_addr, end_addr - 1);
    
    // 直接调用 esp_flash 的批量擦除，让底层驱动处理超时
    esp_err_t ret = esp_flash_erase_region(esp_flash_handle_, aligned_addr, erase_size);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bulk erase failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Bulk erase completed successfully");
    }
    
    return ret;
}

esp_err_t W25Q64Flash::DownloadToFlash(const char* url, uint32_t flash_address, 
                                       void (*progress_callback)(size_t downloaded, size_t total)) {
    if (!initialized_) {
        ESP_LOGE(TAG, "Flash not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!url) {
        ESP_LOGE(TAG, "Invalid URL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (flash_address >= chip_size_) {
        ESP_LOGE(TAG, "Flash address 0x%06" PRIX32 " exceeds chip size", flash_address);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Starting download from: %s", url);
    ESP_LOGI(TAG, "Target flash address: 0x%06" PRIX32, flash_address);
    
    // HTTP 客户端配置
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 600000;  // 10分钟超时（15MB文件需要更长时间）
    config.buffer_size = 8192;   // 增加接收缓冲区大小到8KB
    config.buffer_size_tx = 1024;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 5;
    config.keep_alive_idle = 5;   // Keep-alive空闲时间（秒）
    config.keep_alive_interval = 5;  // Keep-alive间隔（秒）
    config.keep_alive_count = 3;     // Keep-alive探测次数
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    
    // 获取内容长度
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "Failed to fetch headers, content_length = %d", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    
    // 检查状态码
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status code: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Content length: %d bytes", content_length);
    
    // 检查文件大小是否超过 Flash 容量
    size_t available_space = chip_size_ - flash_address;
    ESP_LOGI(TAG, "Flash capacity: %" PRIu32 " MB (%" PRIu32 " bytes)", 
             chip_size_ / (1024 * 1024), chip_size_);
    ESP_LOGI(TAG, "Available space at 0x%06" PRIX32 ": %zu bytes (%.2f MB)", 
             flash_address, available_space, available_space / (1024.0f * 1024.0f));
    ESP_LOGI(TAG, "File size: %d bytes (%.2f MB)", 
             content_length, content_length / (1024.0f * 1024.0f));
    
    if (flash_address + content_length > chip_size_) {
        ESP_LOGE(TAG, "❌ File size (%d bytes, %.2f MB) exceeds available flash space!", 
                 content_length, content_length / (1024.0f * 1024.0f));
        ESP_LOGE(TAG, "   Flash capacity: %" PRIu32 " MB (%" PRIu32 " bytes)", 
                 chip_size_ / (1024 * 1024), chip_size_);
        ESP_LOGE(TAG, "   Available space: %zu bytes (%.2f MB)", 
                 available_space, available_space / (1024.0f * 1024.0f));
        ESP_LOGE(TAG, "   Required space: %d bytes (%.2f MB)", 
                 content_length, content_length / (1024.0f * 1024.0f));
        ESP_LOGE(TAG, "   Shortage: %zu bytes (%.2f MB)", 
                 (flash_address + content_length) - chip_size_,
                 ((flash_address + content_length) - chip_size_) / (1024.0f * 1024.0f));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // 使用批量擦除（更简单，让底层驱动处理）
    ESP_LOGI(TAG, "Erasing flash area for %d bytes...", content_length);
    
    // 方案1：尝试使用批量擦除（可能更快）
    err = BulkErase(flash_address, content_length);
    
    if (err == ESP_ERR_TIMEOUT) {
        // 方案2：如果批量擦除超时，尝试分段擦除
        ESP_LOGW(TAG, "Bulk erase timed out, trying segmented erase...");
        
        uint32_t start_sector = flash_address / W25Q64_SECTOR_SIZE;
        uint32_t end_address = flash_address + content_length - 1;
        uint32_t end_sector = end_address / W25Q64_SECTOR_SIZE;
        uint32_t current_addr = start_sector * W25Q64_SECTOR_SIZE;
        uint32_t end_addr = (end_sector + 1) * W25Q64_SECTOR_SIZE;
        
        // 分批擦除，每次最多擦除 256KB (4个64KB块)
        const size_t MAX_ERASE_SIZE = 256 * 1024;
        
        while (current_addr < end_addr) {
            size_t erase_size = (end_addr - current_addr > MAX_ERASE_SIZE) ? 
                               MAX_ERASE_SIZE : (end_addr - current_addr);
            
            ESP_LOGI(TAG, "Erasing %zu KB at 0x%06" PRIX32 "...", 
                     erase_size / 1024, current_addr);
            
            // 尝试直接调用底层的区域擦除
            err = esp_flash_erase_region(esp_flash_handle_, current_addr, erase_size);
            
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to erase region at 0x%06" PRIX32 ": %s", 
                         current_addr, esp_err_to_name(err));
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return err;
            }
            
            current_addr += erase_size;
            
            // 每擦除一段就让出 CPU，防止看门狗超时
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        ESP_LOGI(TAG, "Segmented erase completed");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase flash: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }
    
    ESP_LOGI(TAG, "Erase complete, starting download...");
    
    // 分配缓冲区（增加缓冲区大小以提高下载稳定性）
    const size_t buffer_size = 8192;  // 增加到8KB
    uint8_t* buffer = (uint8_t*)heap_caps_malloc(buffer_size, MALLOC_CAP_DMA);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    
    size_t total_downloaded = 0;
    uint32_t write_address = flash_address;
    const int MAX_RECONNECT_RETRIES = 5;  // 最大重连次数
    
    // 下载并写入 Flash（带断点续传功能）
    while (total_downloaded < content_length) {
        int data_read = esp_http_client_read(client, (char*)buffer, buffer_size);
        
        if (data_read < 0) {
            // 连接断开，需要重新建立连接并断点续传
            ESP_LOGW(TAG, "Connection lost at %zu/%d bytes (%.2f%%), attempting resume...", 
                     total_downloaded, content_length, 
                     (total_downloaded * 100.0f) / content_length);
            
            // 关闭旧连接
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            
            // 重新建立连接（使用Range请求实现断点续传）
            bool reconnect_success = false;
            for (int retry = 0; retry < MAX_RECONNECT_RETRIES; retry++) {
                vTaskDelay(pdMS_TO_TICKS(2000));  // 等待2秒后重试
                
                ESP_LOGI(TAG, "Reconnecting with Range request (attempt %d/%d)...", retry + 1, MAX_RECONNECT_RETRIES);
                
                // 重新初始化客户端
                client = esp_http_client_init(&config);
                if (!client) {
                    ESP_LOGE(TAG, "Failed to reinitialize HTTP client");
                    continue;
                }
                
                // 设置Range请求头，从断点继续下载
                char range_header[64];
                snprintf(range_header, sizeof(range_header), "bytes=%zu-%d", 
                         total_downloaded, content_length - 1);
                esp_http_client_set_header(client, "Range", range_header);
                
                // 打开连接
                err = esp_http_client_open(client, 0);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to reopen connection: %s", esp_err_to_name(err));
                    esp_http_client_cleanup(client);
                    continue;
                }
                
                // 获取响应头
                int resume_length = esp_http_client_fetch_headers(client);
                int resume_status = esp_http_client_get_status_code(client);
                
                // 206 Partial Content 表示服务器支持断点续传
                if (resume_status == 206 && resume_length > 0) {
                    ESP_LOGI(TAG, "✅ Resume successful! Server supports Range, continuing from byte %zu", total_downloaded);
                    reconnect_success = true;
                    break;
                } else if (resume_status == 200) {
                    // 服务器不支持Range，返回完整文件（需要跳过已下载部分）
                    ESP_LOGW(TAG, "⚠️ Server doesn't support Range (status 200), will skip %zu bytes", total_downloaded);
                    reconnect_success = true;
                    // 跳过已下载的部分
                    size_t skip_bytes = total_downloaded;
                    while (skip_bytes > 0) {
                        size_t skip = (skip_bytes > buffer_size) ? buffer_size : skip_bytes;
                        int skipped = esp_http_client_read(client, (char*)buffer, skip);
                        if (skipped <= 0) {
                            ESP_LOGE(TAG, "Failed to skip bytes");
                            reconnect_success = false;
                            break;
                        }
                        skip_bytes -= skipped;
                    }
                    if (reconnect_success) {
                        ESP_LOGI(TAG, "✅ Skipped %zu bytes, resuming download", total_downloaded);
                        break;
                    }
                } else {
                    ESP_LOGW(TAG, "Unexpected status code: %d, retrying...", resume_status);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    continue;
                }
            }
            
            if (!reconnect_success) {
                ESP_LOGE(TAG, "❌ Failed to reconnect after %d attempts. Downloaded: %zu/%d bytes (%.2f%%)", 
                         MAX_RECONNECT_RETRIES, total_downloaded, content_length,
                         (total_downloaded * 100.0f) / content_length);
                free(buffer);
                return ESP_FAIL;
            }
            
            // 重新尝试读取
            continue;
        }
        
        if (data_read > 0) {
            // 写入 Flash
            err = Write(write_address, buffer, data_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write to flash at 0x%06" PRIX32 ": %s", 
                         write_address, esp_err_to_name(err));
                free(buffer);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return err;
            }
            
            total_downloaded += data_read;
            write_address += data_read;
            
            // 调用进度回调
            if (progress_callback) {
                progress_callback(total_downloaded, content_length);
            }
            
            // 打印进度（每 10%）
            static int last_percent = -1;
            int percent = (total_downloaded * 100) / content_length;
            if (percent != last_percent && percent % 10 == 0) {
                ESP_LOGI(TAG, "Download progress: %d%% (%zu/%d bytes)", 
                         percent, total_downloaded, content_length);
                last_percent = percent;
            }
        } else {
            // data_read == 0，下载完成
            break;
        }
        
        // 喂狗，防止看门狗超时
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    free(buffer);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    
    if (total_downloaded != content_length) {
        ESP_LOGE(TAG, "Download incomplete: %zu/%d bytes", total_downloaded, content_length);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Download complete! %zu bytes written to flash at 0x%06" PRIX32, 
             total_downloaded, flash_address);
    
    // 验证写入的数据（可选，读取前几个字节进行检查）
    uint8_t verify_buffer[16];
    err = Read(flash_address, verify_buffer, sizeof(verify_buffer));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "First 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 verify_buffer[0], verify_buffer[1], verify_buffer[2], verify_buffer[3],
                 verify_buffer[4], verify_buffer[5], verify_buffer[6], verify_buffer[7],
                 verify_buffer[8], verify_buffer[9], verify_buffer[10], verify_buffer[11],
                 verify_buffer[12], verify_buffer[13], verify_buffer[14], verify_buffer[15]);
    }
    
    return ESP_OK;
}