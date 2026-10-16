#include "caps.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_app_desc.h"

const char *caps_chip_name(void)
{
    static char name[24];
    if (!name[0]) {
        /* "esp32s3" -> "ESP32-S3" */
        const char *t = CONFIG_IDF_TARGET;
        size_t n = 0;
        for (size_t i = 0; t[i] && n < sizeof(name) - 2; i++) {
            if (i == 5 && isalpha((unsigned char)t[i]) && isdigit((unsigned char)t[i - 1])) {
                name[n++] = '-';
            }
            name[n++] = (char)toupper((unsigned char)t[i]);
        }
        name[n] = '\0';
    }
    return name;
}

static void add_flag(cJSON *obj, const char *key, bool on)
{
    cJSON_AddBoolToObject(obj, key, on);
}

static const char *ulp_kind(void)
{
#if CONFIG_ULP_COPROC_ENABLED
#if CONFIG_ULP_COPROC_TYPE_FSM
    return "ULP-FSM";
#elif CONFIG_ULP_COPROC_TYPE_RISCV
    return "ULP-RISC-V";
#elif CONFIG_ULP_COPROC_TYPE_LP_CORE
    return "LP-Core";
#endif
#endif
    if (CAP_DEFINED(SOC_LP_CORE_SUPPORTED)) return "LP-Core (disabled in build)";
    if (CAP_DEFINED(SOC_RISCV_COPROC_SUPPORTED)) return "ULP-RISC-V (disabled in build)";
    if (CAP_DEFINED(SOC_ULP_FSM_SUPPORTED)) return "ULP-FSM (disabled in build)";
    return "none";
}

cJSON *caps_info_json(void)
{
    cJSON *root = cJSON_CreateObject();

    esp_chip_info_t ci;
    esp_chip_info(&ci);

    cJSON *chip = cJSON_AddObjectToObject(root, "chip");
    cJSON_AddStringToObject(chip, "name", caps_chip_name());
    cJSON_AddStringToObject(chip, "target", CONFIG_IDF_TARGET);
    char rev[16];
    snprintf(rev, sizeof(rev), "v%d.%d", ci.revision / 100, ci.revision % 100);
    cJSON_AddStringToObject(chip, "revision", rev);
    cJSON_AddNumberToObject(chip, "cores", ci.cores);
    cJSON_AddNumberToObject(chip, "cpu_mhz", esp_rom_get_cpu_ticks_per_us());
    cJSON_AddBoolToObject(chip, "embedded_flash", (ci.features & CHIP_FEATURE_EMB_FLASH) != 0);
    cJSON_AddBoolToObject(chip, "embedded_psram", (ci.features & CHIP_FEATURE_EMB_PSRAM) != 0);

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) == ESP_OK) {
        char s[20];
        snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(chip, "mac", s);
    }

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *fw = cJSON_AddObjectToObject(root, "firmware");
    cJSON_AddStringToObject(fw, "version", app->version);
    cJSON_AddStringToObject(fw, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(fw, "built", app->date);
#if CONFIG_COMPILER_OPTIMIZATION_PERF
    cJSON_AddStringToObject(fw, "optimization", "-O2");
#elif CONFIG_COMPILER_OPTIMIZATION_SIZE
    cJSON_AddStringToObject(fw, "optimization", "-Os");
#elif CONFIG_COMPILER_OPTIMIZATION_NONE
    cJSON_AddStringToObject(fw, "optimization", "-O0");
#else
    cJSON_AddStringToObject(fw, "optimization", "-Og");
#endif

    /* Memory */
    cJSON *mem = cJSON_AddObjectToObject(root, "memory");
    cJSON_AddNumberToObject(mem, "internal_total", (double)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "psram_total", (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    uint32_t flash_phys = 0, flash_id = 0;
    esp_flash_get_physical_size(NULL, &flash_phys);
    esp_flash_read_id(NULL, &flash_id);
    cJSON_AddNumberToObject(mem, "flash_physical", flash_phys);
    cJSON_AddNumberToObject(mem, "flash_id", flash_id);

    /* Features: what this silicon can do (drives which benchmarks are offered) */
    cJSON *f = cJSON_AddObjectToObject(root, "features");
    add_flag(f, "wifi", CAP_DEFINED(SOC_WIFI_SUPPORTED));
    add_flag(f, "bt", CAP_DEFINED(SOC_BT_SUPPORTED));
    add_flag(f, "ble", CAP_DEFINED(SOC_BLE_SUPPORTED));
    add_flag(f, "ieee802154", CAP_DEFINED(SOC_IEEE802154_SUPPORTED));
    add_flag(f, "psram_capable", CAP_DEFINED(SOC_SPIRAM_SUPPORTED));
    add_flag(f, "temp_sensor", CAP_DEFINED(SOC_TEMP_SENSOR_SUPPORTED));
    add_flag(f, "touch", CAP_DEFINED(SOC_TOUCH_SENSOR_SUPPORTED));
    add_flag(f, "dac", CAP_DEFINED(SOC_DAC_SUPPORTED));
    add_flag(f, "rmt", CAP_DEFINED(SOC_RMT_SUPPORTED));
    add_flag(f, "ledc", CAP_DEFINED(SOC_LEDC_SUPPORTED));
    add_flag(f, "i2s", CAP_DEFINED(SOC_I2S_SUPPORTED));
    add_flag(f, "twai", CAP_DEFINED(SOC_TWAI_SUPPORTED));
    add_flag(f, "mcpwm", CAP_DEFINED(SOC_MCPWM_SUPPORTED));
    add_flag(f, "pcnt", CAP_DEFINED(SOC_PCNT_SUPPORTED));
    add_flag(f, "sdmmc", CAP_DEFINED(SOC_SDMMC_HOST_SUPPORTED));
    add_flag(f, "emac", CAP_DEFINED(SOC_EMAC_SUPPORTED));
    add_flag(f, "usb_otg", CAP_DEFINED(SOC_USB_OTG_SUPPORTED));
    add_flag(f, "usb_serial_jtag", CAP_DEFINED(SOC_USB_SERIAL_JTAG_SUPPORTED));
    cJSON_AddStringToObject(f, "ulp", ulp_kind());

    /* Crypto / security blocks */
    cJSON *c = cJSON_AddObjectToObject(root, "crypto");
    add_flag(c, "aes", CAP_DEFINED(SOC_AES_SUPPORTED));
    add_flag(c, "sha", CAP_DEFINED(SOC_SHA_SUPPORTED));
    add_flag(c, "rsa_mpi", CAP_DEFINED(SOC_MPI_SUPPORTED));
    add_flag(c, "ecc", CAP_DEFINED(SOC_ECC_SUPPORTED));
    add_flag(c, "hmac", CAP_DEFINED(SOC_HMAC_SUPPORTED));
    add_flag(c, "digital_signature", CAP_DEFINED(SOC_DIG_SIGN_SUPPORTED));
    add_flag(c, "flash_encryption", CAP_DEFINED(SOC_FLASH_ENC_SUPPORTED));
    add_flag(c, "secure_boot", CAP_DEFINED(SOC_SECURE_BOOT_SUPPORTED));
    add_flag(c, "xts_aes", CAP_DEFINED(SOC_AES_SUPPORT_XTS) || CAP_DEFINED(SOC_FLASH_ENCRYPTION_XTS_AES));

    /* Defaults the UI pre-fills into its inputs */
    cJSON *d = cJSON_AddObjectToObject(root, "defaults");
    cJSON_AddNumberToObject(d, "uart_tx", CONFIG_WEBDBG_UART_TX_GPIO);
    cJSON_AddNumberToObject(d, "uart_rx", CONFIG_WEBDBG_UART_RX_GPIO);
    cJSON_AddNumberToObject(d, "i2c_sda", CONFIG_WEBDBG_I2C_SDA_GPIO);
    cJSON_AddNumberToObject(d, "i2c_scl", CONFIG_WEBDBG_I2C_SCL_GPIO);
    cJSON_AddNumberToObject(d, "led_gpio", CONFIG_WEBDBG_BENCH_LED_GPIO);
    cJSON_AddNumberToObject(d, "rgb_gpio", CONFIG_WEBDBG_BENCH_RGB_GPIO);

    return root;
}
