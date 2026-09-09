#include <stdio.h>
#include <Preferences.h>
#include <string.h>

#include "multiboot.h"
#include "fileutils.h"
#include "serial.h"
#include <esp_crc.h>
#include <esp_rom_md5.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_flash_partitions.h>

#define MULTIBOOT_KCMD_DEFAULT_LOCATION 0x50000000
typedef struct {
    char cmd[MULTIBOOT_CMD_LEN];
    uint32_t crc;
} KernelParams;

RTC_DATA_ATTR KernelParams kcmd;

extern "C" bool verifyRollbackLater() {
    return true;
}

namespace lilka {

extern FileUtils fileutils;

#define MULTIBOOT_PATH_KEY "multiboot_path"

MultiBoot::MultiBoot() :
    ota_handle(0), current_partition(NULL), ota_partition(NULL), path(""), bytesTotal(0), bytesWritten(0), file(NULL) {
}

void MultiBoot::begin() {
    // Якщо кейра колись почне давати користувачам SPIFFS
    // то треба буде встановлювати активним сегмент #0
    // станом на зараз можна тримати закоментованим
    // if (getActiveSSPIFFSSegment() > 0) {
    //     if (setActiveSSPIFFSSegment(0) == ESP_OK) {
    //         esp_restart();
    //     }
    // }

    // Get commandline args
    bool verify_kcmd_loc = &kcmd == reinterpret_cast<KernelParams*>(MULTIBOOT_KCMD_DEFAULT_LOCATION);
    if (!verify_kcmd_loc) {
        lilka::serial.err(
            "kernel cmd parameters structure located in unexpected place %p. Default location %x",
            &kcmd,
            MULTIBOOT_KCMD_DEFAULT_LOCATION
        );
        lilka::serial.err("kernel commandline parameters would be ignored");
    } else {
        // Verify commandline crc
        uint32_t cmdcrc = esp_crc32_le(0, reinterpret_cast<uint8_t*>(kcmd.cmd), MULTIBOOT_CMD_LEN);

        bool verify_cmd_crc = cmdcrc == kcmd.crc;
        if (verify_cmd_crc) {
            lilka::serial.log("Boot with cmd params");
            lilka::serial.log(kcmd.cmd);

            // count argc
            int count = 0;

            const char* p = kcmd.cmd;
            while (*p) {
                while (*p == ' ')
                    p++;
                if (!*p) break;
                count++;
                while (*p && *p != ' ')
                    p++;
            }
            argc = count;
            argv = reinterpret_cast<char**>(malloc(sizeof(char*) * (count + 1)));

            // split
            int i = 0;
            char* c = kcmd.cmd;
            while (*c) {
                while (*c == ' ')
                    c++;
                if (!*c) break;
                argv[i++] = c;
                while (*c && *c != ' ')
                    c++;
                if (*c) *c++ = '\0';
            }
            argv[i] = NULL;
            // after we made a split in iram we can be sure that our crc now is a joke
        }
    }
    current_partition = esp_ota_get_running_partition();
    serial.log(
        "Current partition: %s, type: %d, subtype: %d, size: %d",
        current_partition->label,
        current_partition->type,
        current_partition->subtype,
        current_partition->size
    );
    ota_partition = esp_ota_get_next_update_partition(current_partition); // get ota1 (we're in ota0 now)
    serial.log(
        "OTA partition: %s, type: %d, subtype: %d, size: %d",
        ota_partition->label,
        ota_partition->type,
        ota_partition->subtype,
        ota_partition->size
    );

    // А тут я згаяв трохи часу. Нижче наведено спроби увімкнути автоматичний відкат прошивки з кінцевим рішенням.

    // Спроба 1:

    /*
    // Check if rollback is possible
    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state);
    if (err != ESP_OK) {
        serial.err("Failed to get state partition: %d", err);
        return;
    }

    serial.log("OTA state: %d", ota_state);

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        serial.log("Rollback is possible");
        // Mark ota0 as active partition so that we return to main application after next restart
        esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL));
    } else {
        serial.log("Rollback is not possible");
    }
    */

    // Спроба 2:
    // Auto-rollback does not work properly - my dev board's bootloader might be messing it up,
    // since every OTA update is marked as successful, even if we don't mark it as valid
    // TODO: Maybe it will work with actual Lilka v2?
    // So here's a workaround: I'm going to set ota0 as active partition anyway, so that we return to main application after next restart.

    // Mark ota0 as active partition so that we return to main application after next restart
    // esp_ota_img_states_t ota_state;
    // esp_err_t err = esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state);
    // serial.log("OTA state: %d", ota_state);
    // if (err != ESP_OK) {
    //     serial.err("Failed to get state partition: %d", err);
    //     return;
    // }
    // if (current_partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
    //     esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL));
    // }

    // Спроба 3:
    // Ок, розібрався. (Так, я по звичці писав попередні коментарі англійською мовою, але намагаюсь використовувати українську.)
    // Потрібно було перевизначити verifyRollbackLater(), щоб він завжди повертав false, тоді автоматичний відкат працюватиме.
    // https://github.com/espressif/arduino-esp32/issues/7423

    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state);
    serial.log("OTA state: %d", ota_state);
}

int MultiBoot::start(String path) {
    // Завантаження прошивки з microSD-картки.
    this->path = path;

    if (!fileutils.isSDAvailable()) {
        serial.err("SD card not available");
        return -1;
    }

    // String abspath = sdcard.abspath(path);

    // TODO: Use sdcard instead of SD
    file = fopen(path.c_str(), "r");
    if (file == NULL) {
        serial.err("Failed to open file: %s", path.c_str());
        return -2;
    }

    bytesWritten = 0;
    // Get file size
    fseek(file, 0, SEEK_END);
    bytesTotal = ftell(file);
    fseek(file, 0, SEEK_SET);

    current_partition = esp_ota_get_running_partition();
    if (current_partition == NULL) {
        serial.err("Failed to get current partition");
        return -3;
    }
    serial.log(
        "Current partition: %s, type: %d, subtype: %d, size: %d",
        current_partition->label,
        current_partition->type,
        current_partition->subtype,
        current_partition->size
    );
    ota_partition = esp_ota_get_next_update_partition(current_partition); // get ota1 (we're in ota0 now)
    if (ota_partition == NULL) {
        serial.err("Failed to get next OTA partition");
        return -4;
    }
    serial.log(
        "OTA partition: %s, type: %d, subtype: %d, size: %d",
        ota_partition->label,
        ota_partition->type,
        ota_partition->subtype,
        ota_partition->size
    );

    esp_err_t err = esp_ota_begin(ota_partition, bytesTotal, &ota_handle);
    if (err != ESP_OK) {
        serial.err("Failed to begin OTA: %d", err);
        return -5;
    }

    Preferences prefs;
    prefs.begin("lilka", false);
    String arg = path;
    // Remove "/sd" prefix
    // TODO: Maybe we should use absolute path (including "/sd")?
    // TODO: Store arg in RAM?
    arg = lilka::fileutils.getLocalPathInfo(arg).path;

    prefs.putString(MULTIBOOT_PATH_KEY, arg);
    prefs.end();

    return 0;
}

int MultiBoot::process() {
    char buf[4096];

    // Записуємо 16 КБ.

    for (int i = 0; i < 4; i++) {
        // Read 1024 bytes
        int len = fread(buf, 1, sizeof(buf), file);
        if (len == 0) {
            fclose(file);
            return 0;
        }

        esp_err_t err = esp_ota_write(ota_handle, buf, len);
        if (err != ESP_OK) {
            serial.err("Failed to write OTA: %d", err);
            return -6;
        }

        bytesWritten += len;
    }

    return bytesWritten;
}

void MultiBoot::cancel() {
    if (file != NULL) {
        fclose(file);
    }
    if (ota_handle) {
        esp_ota_abort(ota_handle);
        ota_handle = 0;
    }
}

int MultiBoot::getBytesTotal() {
    return bytesTotal;
}

int MultiBoot::getBytesWritten() {
    return bytesWritten;
}

int MultiBoot::finishAndReboot() {
    serial.log("Written %d bytes", bytesWritten);

    esp_err_t err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        serial.err("Failed to end OTA: %d", err);
        return -7;
    }

    // Перевстановлення активного розділу на OTA-розділ (його буде запущено лише один раз, після чого активним залишиться основний розділ).
    err = esp_ota_set_boot_partition(ota_partition);
    if (err != ESP_OK) {
        serial.err("Failed to set boot partition: %d", err);
        return -8;
    }

    // Запуск нової прошивки.
    bootLast();

    return 0; // unreachable
}

void MultiBoot::bootLast() {
    auto err = esp_ota_set_boot_partition(ota_partition);
    if (err != ESP_OK) {
        serial.err("Failed to set boot partition: %d", err);
    }
    // Також активуєм відповідний сегмент SPIFFFS
    int segment = segmentForOTAFirmware(getFirmwarePath());
    if (segment >= 0 && getActiveSSPIFFSSegment() != segment) {
        setActiveSSPIFFSSegment(segment);
    }

    esp_restart();
}

String MultiBoot::getFirmwarePath() {
    Preferences prefs;
    prefs.begin("lilka", false);
    String arg = "";
    if (prefs.isKey(MULTIBOOT_PATH_KEY)) {
        arg = prefs.getString(MULTIBOOT_PATH_KEY);
        prefs.remove(MULTIBOOT_PATH_KEY);
    }
    prefs.end();
    return arg;
}

// Not defined in some ESP32 SDKs
#define ESP_PARTITION_TYPE_PARTITION_TABLE 0x03
#define ESP_PARTITION_SUBTYPE_PARTITION_TABLE_PRIMARY 0x00
#define ESP_PARTITION_TABLE_SIZE (0x1000)

static int readPartitionTable(uint8_t *table) {

    const esp_partition_t *pt =
        esp_partition_find_first(
            (esp_partition_type_t)ESP_PARTITION_TYPE_PARTITION_TABLE,
            (esp_partition_subtype_t)ESP_PARTITION_SUBTYPE_PARTITION_TABLE_PRIMARY,
            NULL);
    if (pt == NULL) {
        serial.err("Can't find the primary partition table");
        return ESP_ERR_NOT_FOUND;
    }

    return esp_partition_read(pt, 0, table,  ESP_PARTITION_TABLE_SIZE);
}

static int writePartitionTable(uint8_t *table) {
    // Шукаєм таблицю розділів
    const esp_partition_t *pt =
        esp_partition_find_first(
            (esp_partition_type_t)ESP_PARTITION_TYPE_PARTITION_TABLE,
            (esp_partition_subtype_t)ESP_PARTITION_SUBTYPE_PARTITION_TABLE_PRIMARY,
            NULL);
    if (pt == NULL) {
        serial.err("Can't find the primary partition table");
        return ESP_ERR_NOT_FOUND;
    }

    // Рахуєм новий md5 (0x000 ... 0xBFF)
    uint8_t digest[16];
    md5_context_t md5_ctx;

    esp_rom_md5_init(&md5_ctx);
    esp_rom_md5_update(&md5_ctx, table, ESP_PARTITION_TABLE_MAX_LEN);
    esp_rom_md5_final(digest, &md5_ctx);

    esp_partition_info_t *md5entry = (esp_partition_info_t *)(table + ESP_PARTITION_TABLE_MAX_LEN);

    memset(md5entry, 0xFF, sizeof(esp_partition_info_t));
    md5entry->magic = ESP_PARTITION_MAGIC_MD5;

    // пишем md5 digest
    memcpy(
        table + ESP_PARTITION_TABLE_MAX_LEN + ESP_PARTITION_MD5_OFFSET,
        digest,
        sizeof(digest));

    // перезаписуєм розділ
    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(pt, ESP_PARTITION_TABLE_SIZE, &handle);
    if (err != ESP_OK) return err;
    err = esp_ota_write(handle, table, ESP_PARTITION_TABLE_SIZE);
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        return err;
    }

    return esp_ota_end(handle);
}

int MultiBoot::getActiveSSPIFFSSegment() {

    uint8_t table[ESP_PARTITION_TABLE_SIZE];
    if (readPartitionTable(table) != ESP_OK) return 0;
        
    // Шукаєм SPIFFS
    esp_partition_info_t *spiffs = NULL;
    for (size_t i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; i++) {

        esp_partition_info_t *entry = (esp_partition_info_t *)(
            table + i * sizeof(esp_partition_info_t));
        if (entry->type == ESP_PARTITION_TYPE_DATA && entry->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) {
            spiffs = entry;
            break;
        }
    }
    
    if (!spiffs) {
        serial.err("Can't find any SPIFFS partition");
        return ESP_ERR_NOT_FOUND; // Поганий сценарій, коли якась OTA-прошивка змінила тип розділу
    }

    // SPIFFS знайдено. 
    esp_partition_pos_t pos = spiffs->pos;
    // Перевіряємо, чи розділ сегментований
    if (pos.offset == MULTIBOOT_SPIFFS_BEGIN && pos.size == MULTIBOOT_SPIFFS_SIZE) {
        return -1;
    }

    // Рахуємо активний сегмент
    int segSize = MULTIBOOT_SPIFFS_SIZE / MULTIBOOT_SPIFFS_SEGMENTS;
    int segment = (pos.offset - MULTIBOOT_SPIFFS_BEGIN) / segSize;
    serial.log("Active SPIFFFS segment: %d", segment);

    return segment;
}

int MultiBoot::setActiveSSPIFFSSegment(int segment) {

    uint8_t table[ESP_PARTITION_TABLE_SIZE];
    if (readPartitionTable(table) != ESP_OK) return 0;
        
    // Шукаєм SPIFFS
    esp_partition_info_t *spiffs = NULL;
    for (size_t i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; i++) {

        esp_partition_info_t *entry = (esp_partition_info_t *)(
            table + i * sizeof(esp_partition_info_t));
        if (entry->type == ESP_PARTITION_TYPE_DATA && entry->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) {
            spiffs = entry;
            break;
        }
    }
    
    if (!spiffs) {
        serial.err("Can't find any SPIFFS partition");
        return ESP_ERR_NOT_FOUND; // Поганий сценарій, коли якась OTA-прошивка змінила тип розділу
    }

    int segSize = MULTIBOOT_SPIFFS_SIZE / MULTIBOOT_SPIFFS_SEGMENTS;
    spiffs->pos.offset = MULTIBOOT_SPIFFS_BEGIN + segment * segSize;
    spiffs->pos.size = segSize;

    int err = writePartitionTable(table);
    if (err != ESP_OK) {
        serial.err("Can't write Partition table: %d", err);
    } else {
        serial.log("Activated SPIFFFS segment: %d", segment);
        // Тепер готові до ребуту
    }
    return err;
}

int MultiBoot::segmentForOTAFirmware(String path) {
    // Вирахування сегменту в залежності від імені прошивки
    // Для простоти та зворотньої сумісності:
    // *.bin0, Keira -> 0 
    // *.bin, *.bin1 -> 1
    // *.bin2 -> 2, і так далі
    path.toLowerCase();
    if (path.endsWith("bin")) return 1;

    int fileExtPos = path.lastIndexOf("bin");
    if (fileExtPos > 0) {
        long num = path.substring(fileExtPos+3).toInt();
        if (num < MULTIBOOT_SPIFFS_SEGMENTS) return num;
    }

    serial.err("Can't determine SPIFFS segment. Using Keira default");
    return 0;
}


void MultiBoot::setCMDParams(String cmd) {
    if (cmd.length() > MULTIBOOT_CMD_LEN) {
        serial.err("Too long commandline for kernel set. Consider enlarging MULTIBOOT_CMD_LEN");
        return; // get out
    }
    // ZERO MEM
    memset(kcmd.cmd, 0, MULTIBOOT_CMD_LEN);
    kcmd.crc = 0;

    // copying parameter
    memcpy(kcmd.cmd, cmd.c_str(), cmd.length());

    // Calculating crc 32
    kcmd.crc = esp_crc32_le(0, reinterpret_cast<uint8_t*>(kcmd.cmd), MULTIBOOT_CMD_LEN);
}
int MultiBoot::getArgc() {
    return argc;
}

char** MultiBoot::getArgv() {
    return argv;
}

esp_reset_reason_t MultiBoot::getResetReason() {
    return esp_reset_reason();
}

MultiBoot multiboot;

} // namespace lilka
