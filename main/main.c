#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "esp_vfs.h"
#include "esp_http_server.h"
#include "mdns.h"

#define WIFI_SSID   "softap"
#define WIFI_PWORD  "coolbeans"
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 32)
#define REQ_BUF_SIZE (10240)

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

static const char *TAG = "ncdanie";
static const char *BASE_PATH = "/www";

typedef struct usr_req_ctx {
    char buffer[REQ_BUF_SIZE];
} usr_req_ctx_t;

esp_err_t init_spiffs();
void start_server(httpd_handle_t server);
void stop_server(httpd_handle_t server);
esp_err_t index_get_handler(httpd_req_t *req);
esp_err_t hello_get_handler(httpd_req_t *req);

static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath)
{
    const char *type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "text/xml";
    }
    return httpd_resp_set_type(req, type);
}

static void ap_start_event_handler(
    void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    start_server(arg);
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set("ncdanie");
    mdns_instance_name_set("wifi provisioning server");
}

static void ap_stop_event_handler(
    void *arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    stop_server(arg);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES
        || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(init_spiffs());

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_ERROR_CHECK(ret);
    }

    // The returned netif instance must be destroyed
    // if the wifi is de-initialized
    esp_netif_create_default_wifi_ap();

    // Init wifi with the default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    httpd_handle_t http_server = NULL;
    esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_START,
        &ap_start_event_handler, http_server, NULL
    );
    esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_AP_STOP,
        &ap_stop_event_handler, http_server, NULL
    );

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {
                .required = true,
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    int fd = open("/www/index.html", O_RDONLY);
    if (fd != -1) {
        close(fd);
        ESP_LOGI(TAG, "Successfully opened index.html");
    }
}

esp_err_t init_spiffs()
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = BASE_PATH,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}

void start_server(httpd_handle_t server)
{
    ESP_LOGI(TAG, "Starting http server");
    usr_req_ctx_t *req_ctx = calloc(1, sizeof(usr_req_ctx_t));
    if (!(req_ctx)) {
        ESP_LOGE(TAG, "No memory for user context");
    }
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.lru_purge_enable = true;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_uri_t get_uri = {
        .uri        = "/*",
        .method     = HTTP_GET,
        .handler    = index_get_handler,
        .user_ctx   = req_ctx,
    };

    if(httpd_start(&server, &http_cfg) == ESP_OK) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_uri));
    } else {
        free(req_ctx);
    }
}

void stop_server(httpd_handle_t server)
{
    if (server != NULL)
    {
        ESP_ERROR_CHECK(httpd_stop(server));
    }
}

esp_err_t index_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];

    ESP_LOGI(TAG, "Recieved request for URI : %s", req->uri);

    strlcpy(filepath, BASE_PATH, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/') {
        strlcat(filepath, "/index.html", sizeof(filepath));
    } else {
        strlcat(filepath, req->uri, sizeof(filepath));
    }

    ESP_LOGI(TAG, "Preparing to open %s", filepath);
    int fd = open(filepath, O_RDONLY);
    if (fd == -1) {
        ESP_LOGE(TAG, "Failed to open file : %s", filepath);
        httpd_resp_send_err(
            req, HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to read file"
        );
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Opened file %s", filepath);

    set_content_type_from_file(req, filepath);
    // if (set_content_type_from_file(req, filepath) != ESP_OK) {
    //     httpd_resp_send_err(
    //         req, HTTPD_500_INTERNAL_SERVER_ERROR,
    //         "Failed to set content type"
    //     );
    //     return ESP_FAIL;
    // }

    usr_req_ctx_t *ctx = (usr_req_ctx_t *)req->user_ctx;
    char *chunk = ctx->buffer;
    int bytes_read;
    do {
        bytes_read = read(fd, chunk, REQ_BUF_SIZE);
        if (bytes_read == -1) {
            ESP_LOGE(
                TAG, "Failed to read file : %s", filepath
            );
        } else if (bytes_read > 0) {
            if (httpd_resp_send_chunk(req, chunk, bytes_read) != ESP_OK) {
                close(fd);
                ESP_LOGE(TAG, "Failed to send file chunk");
                httpd_resp_sendstr_chunk(req, NULL);
                httpd_resp_send_err(
                    req, HTTPD_500_INTERNAL_SERVER_ERROR,
                    "Failed to send file"
                );
                return ESP_FAIL;
            }
        }
    } while (bytes_read > 0);
    close(fd);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}
