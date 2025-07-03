#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_check.h"
#include "es8311.h"
#include "mi_audio_config.h"
#include "mi_queue.h"
#include "mi_led.h"

static const char *TAG = "i2s_es8311";
//static const char err_reason[][30] = {"input param is invalid", "operation timeout"};

//--> Handelers para la conexion i2s
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;

//--> Flag de pausa y su mutex
volatile bool paused = false;  // true es pausa, false es resumir
static SemaphoreHandle_t paused_mutex = NULL;

//--> Variable de volumen y su mutex 
volatile uint8_t volume = 60;  // Volumen default (no es lineal)
static uint8_t last_volume = 0;
static es8311_handle_t es_handle = NULL;
static SemaphoreHandle_t volume_mutex = NULL;

//--> Flag para el cambio de música y su mutex
static SemaphoreHandle_t music_change_mutex = NULL;
volatile bool change_track_next = false;
volatile bool change_track_prev = false;

led_strip_t *strip;
int R = 255, G = 0, B = 0;

//--> Añadimos las canciones
extern const uint8_t doom_pcm_start[] asm("_binary_doom_pcm_start");
extern const uint8_t doom_pcm_end[]   asm("_binary_doom_pcm_end");

extern const uint8_t butterfly_pcm_start[] asm("_binary_butterfly_pcm_start");
extern const uint8_t butterfly_pcm_end[]   asm("_binary_butterfly_pcm_end");

/* extern const uint8_t dance_pcm_start[] asm("_binary_dance_pcm_start");
extern const uint8_t dance_pcm_end[]   asm("_binary_dance_pcm_end"); */

extern const uint8_t mission_pcm_start[] asm("_binary_mission_pcm_start");
extern const uint8_t mission_pcm_end[]   asm("_binary_mission_pcm_end");

/* extern const uint8_t tetris_pcm_start[] asm("_binary_tetris_pcm_start");
extern const uint8_t tetris_pcm_end[]   asm("_binary_tetris_pcm_end"); */

extern const uint8_t pacman_pcm_start[] asm("_binary_pacman_pcm_start");
extern const uint8_t pacman_pcm_end[]   asm("_binary_pacman_pcm_end");

extern const uint8_t undertale_pcm_start[] asm("_binary_undertale_pcm_start");
extern const uint8_t undertale_pcm_end[]   asm("_binary_undertale_pcm_end");

//--> Tipo de dato para guardar la cancion
typedef struct {
    const uint8_t *start;
    const uint8_t *end;
} music_track_t;

//--> Creamos el array de canciones 
static const music_track_t tracks[] = {
    { doom_pcm_start, doom_pcm_end },
    { butterfly_pcm_start, butterfly_pcm_end },
    // { dance_pcm_start, dance_pcm_end },
    { mission_pcm_start, mission_pcm_end },
    // { tetris_pcm_start, tetris_pcm_end },
    { pacman_pcm_start, pacman_pcm_end },
    { undertale_pcm_start, undertale_pcm_end }
};

static size_t current_track = 0;
static size_t total_tracks = sizeof(tracks) / sizeof(tracks[0]);

static QueueHandle_t audio_event_queue = NULL;

//--> Esto inicia la comunicacion con la ES8311 a traves de I2C
static esp_err_t es8311_codec_init(void)
{
    /* Initialize I2C peripheral */
    const i2c_config_t es_i2c_cfg = {
        .sda_io_num = I2C_SDA_IO,
        .scl_io_num = I2C_SCL_IO,
        .mode = I2C_MODE_MASTER,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(I2C_NUM, &es_i2c_cfg), TAG, "config i2c failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(I2C_NUM, I2C_MODE_MASTER,  0, 0, 0), TAG, "install i2c driver failed");

    /* Initialize es8311 codec */
    es_handle = es8311_create(I2C_NUM, ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(es_handle, ESP_FAIL, TAG, "es8311 create failed");
    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = EXAMPLE_MCLK_FREQ_HZ,
        .sample_frequency = EXAMPLE_SAMPLE_RATE
    };

    ESP_ERROR_CHECK(es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
    ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(es_handle, EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE, EXAMPLE_SAMPLE_RATE), TAG, "set es8311 sample frequency failed");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es_handle, EXAMPLE_VOICE_VOLUME, NULL), TAG, "set es8311 volume failed");
    ESP_RETURN_ON_ERROR(es8311_microphone_config(es_handle, false), TAG, "set es8311 microphone failed");
    return ESP_OK;
}

//--> Esto inicia el canal i2s para pasarle la musica
static esp_err_t i2s_driver_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = EXAMPLE_MCLK_MULTIPLE;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    return ESP_OK;
}

//--> i2s_music es la task que hace todo (volumen, cambio de cancion y pausa)
static void i2s_music(void *args)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_write = 0;

    uint8_t *data_ptr = (uint8_t *)tracks[current_track].start;
    size_t data_len = tracks[current_track].end - tracks[current_track].start;
    size_t remaining = data_len;

    ESP_ERROR_CHECK(i2s_channel_disable(tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    //--> Definimos variables locales 
    uint8_t local_volume;
    bool local_paused = false;
    bool local_change_track_next = false;
    bool local_change_track_prev = false;
    while (1) {
        //--> Volumen
        if (xSemaphoreTake(volume_mutex, portMAX_DELAY) == pdTRUE) {
            local_volume = volume;
            xSemaphoreGive(volume_mutex);
        } else {
            local_volume = last_volume;
        }
        if (local_volume != last_volume) {
            ESP_LOGI(TAG, "Updating volume: %d", local_volume);
            es8311_voice_volume_set(es_handle, local_volume, NULL);
            last_volume = local_volume;
        }

        //--> Pausa
        if (xSemaphoreTake(paused_mutex, portMAX_DELAY) == pdTRUE) {
            local_paused = paused;
            xSemaphoreGive(paused_mutex);
        } else {
            local_paused = false;
        }

        if (local_paused) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        //--> Cancion siguiente y anterior
        if (xSemaphoreTake(music_change_mutex, portMAX_DELAY) == pdTRUE) {
            //--> Actualizamos flags locales con globales
            local_change_track_next = change_track_next;
            local_change_track_prev = change_track_prev;
            change_track_next = false;
            change_track_prev = false;
            xSemaphoreGive(music_change_mutex);
        }
        //--> Cambiamos la cancion si hace falta
        if (local_change_track_next) {
            current_track = (current_track + 1) % total_tracks;
            ESP_LOGI(TAG, "Track changed to %d (NEXT)", current_track);
        } else if (local_change_track_prev) {
            if (current_track == 0){
                current_track = total_tracks - 1;
            } else {
                current_track--;
            }
            ESP_LOGI(TAG, "Track changed to %d (PREV)", current_track);
        }
        //--> Actualizamos los datos de la cancion si hubo cambio
        if (local_change_track_next || local_change_track_prev) {
            data_ptr = (uint8_t *)tracks[current_track].start;
            data_len = tracks[current_track].end - tracks[current_track].start;
            remaining = data_len;
        }
        //--> Mandamos los datos al parlante
        size_t chunk = 1024;
        if (remaining < chunk) {
            chunk = remaining;
        }
        ret = i2s_channel_write(tx_handle, data_ptr, chunk, &bytes_write, portMAX_DELAY);
        if (ret != ESP_OK || bytes_write == 0) {
            ESP_LOGE(TAG, "[music] i2s write failed or no data written.");
            abort();
        }
        data_ptr += bytes_write;
        remaining -= bytes_write;
        //--> Reinicia la cancion si termina
        if (remaining == 0) {
            data_ptr = (uint8_t *)tracks[current_track].start;
            remaining = data_len;
        }
    }
    vTaskDelete(NULL);
}

//-----------TESTBENCHS LOCALES------------//
void pause_testbench_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (xSemaphoreTake(paused_mutex, portMAX_DELAY) == pdTRUE) {
            paused = true;
            xSemaphoreGive(paused_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (xSemaphoreTake(paused_mutex, portMAX_DELAY) == pdTRUE) {
            paused = false;
            xSemaphoreGive(paused_mutex);
        }
    }
}
void volume_testbench_task(void *arg)
{
    while (1) {
        for (int i = 10; i <= 70; i += 10) {
            volume = i;
            ESP_LOGI("VOL_TEST", "Setting volume: %d", i);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        for (int i = 70; i >= 10; i -= 10) {
            volume = i;
            ESP_LOGI("VOL_TEST", "Setting volume: %d", i);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
void music_change_testbench_task(void *arg)
{
    bool toggle = true; // Alterna entre next y prev

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));  // Cada 10 segundos solicita el cambio

        if (xSemaphoreTake(music_change_mutex, portMAX_DELAY) == pdTRUE) {
            if (toggle) {
                change_track_next = true;
                ESP_LOGI(TAG, "Testbench: Requested track NEXT");
            } else {
                change_track_prev = true;
                ESP_LOGI(TAG, "Testbench: Requested track PREV");
            }
            toggle = !toggle;  // Cambiar para la próxima vez
            xSemaphoreGive(music_change_mutex);
        }
    }
}
//---------TERMINAN LOS TESTBENCHS---------//
void mi_audio_init(void)
{
    led_rgb_init(&strip);

    printf("i2s es8311 codec example start\n-----------------------------\n");
    /* Initialize i2s peripheral */
    if (i2s_driver_init() != ESP_OK) {
        ESP_LOGE(TAG, "i2s driver init failed");
        abort();
    } else {
        ESP_LOGI(TAG, "i2s driver init success");
    }
    /* Initialize i2c peripheral and config es8311 codec by i2c */
    if (es8311_codec_init() != ESP_OK) {
        ESP_LOGE(TAG, "es8311 codec init failed");
        abort();
    } else {
        ESP_LOGI(TAG, "es8311 codec init success");
    }

    /* Enable PA by setting the PA_CTRL_IO to high, because the power amplifier on some dev-kits are disabled by default */
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << EXAMPLE_PA_CTRL_IO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio_cfg));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PA_CTRL_IO, 1));
    //--> Iniciamos los mutex de cada variable
    paused_mutex = xSemaphoreCreateMutex();
    if (paused_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        abort();
    }
    music_change_mutex = xSemaphoreCreateMutex();
    if (music_change_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create music_change_mutex");
        abort();
    }
    volume_mutex = xSemaphoreCreateMutex();
    if (volume_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create volume mutex");
        abort();
    }
    //--> Creamos la tasks de i2s y las taks de los testbench
    xTaskCreate(i2s_music, "i2s_music", 4096, NULL, 5, NULL);
    //xTaskCreate(pause_testbench_task, "pause_test", 2048, NULL, 4, NULL); //TB de pausa
    //xTaskCreate(volume_testbench_task, "volume_test", 2048, NULL, 4, NULL); //TB de volumen
    //xTaskCreate(music_change_testbench_task, "music_change_testbench", 2048, NULL, 4, NULL); //TB de cambio
}

void led_task(void *arg)
{
    bool led_pause = false;
    while (1) {
        if (xSemaphoreTake(music_change_mutex, portMAX_DELAY) == pdTRUE) {
            led_pause = paused;
            xSemaphoreGive(music_change_mutex);
        }
        if (led_pause == false){
            turn_led_on(strip, R, G, B);
            vTaskDelay(pdMS_TO_TICKS(250));
            turn_led_off(strip);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}


static void audio_event_task(void *arg)
{
    mi_evento_t evento;
    while (1) {
        if (mi_queue_receive(audio_event_queue, &evento, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "[AUDIO_EVENT] Recibido evento tipo: %d, value: %d", evento.tipo, evento.value);
            switch (evento.tipo) {
                case EVENT_NEXT_TRACK:
                    ESP_LOGI(TAG, "[AUDIO_EVENT] Siguiente pista");
                    if (xSemaphoreTake(music_change_mutex, portMAX_DELAY) == pdTRUE) {
                        change_track_next = true;
                        xSemaphoreGive(music_change_mutex);
                    }
                    break;
                case EVENT_PREV_TRACK:
                    ESP_LOGI(TAG, "[AUDIO_EVENT] Pista anterior");
                    if (xSemaphoreTake(music_change_mutex, portMAX_DELAY) == pdTRUE) {
                        change_track_prev = true;
                        xSemaphoreGive(music_change_mutex);
                    }
                    break;
                case EVENT_VOL_UP:
                    ESP_LOGI(TAG, "[AUDIO_EVENT] Subir volumen");
                    if (xSemaphoreTake(volume_mutex, portMAX_DELAY) == pdTRUE) {
                        if (volume < 100) volume += 5;
                        xSemaphoreGive(volume_mutex);
                    }
                    break;
                case EVENT_VOL_DOWN:
                    ESP_LOGI(TAG, "[AUDIO_EVENT] Bajar volumen");
                    if (xSemaphoreTake(volume_mutex, portMAX_DELAY) == pdTRUE) {
                        if (volume > 5) volume -= 5;
                        xSemaphoreGive(volume_mutex);
                    }
                    break;
                case EVENT_PLAY_PAUSE:
                    ESP_LOGI(TAG, "[AUDIO_EVENT] Play/Pause");
                    if (xSemaphoreTake(paused_mutex, portMAX_DELAY) == pdTRUE) {
                        paused = !paused;
                        xSemaphoreGive(paused_mutex);
                    }
                    break;
                case EVENT_STOP:
                    ESP_LOGI(TAG, "[AUDIO_EVENT] Stop");
                    // Implementar lógica de stop si es necesario
                    break;
                default:
                    ESP_LOGW(TAG, "[AUDIO_EVENT] Evento desconocido: %d", evento.tipo);
                    break;
            }
        }
    }
}

void mi_audio_init_with_queue(QueueHandle_t queue)
{
    audio_event_queue = queue;
    mi_audio_init();
    // Crear una task para procesar eventos de la queue
    xTaskCreate(audio_event_task, "audio_event_task", 2048, NULL, 5, NULL);
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
}