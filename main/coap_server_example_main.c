/* CoAP server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/*
 * WARNING
 * libcoap is not multi-thread safe, so only this thread must make any coap_*()
 * calls.  Any external (to this thread) data transmitted in/out via libcoap
 * therefore has to be passed in/out by xQueue*() via this thread.
 */

#include <string.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"

#include "nvs_flash.h"

#include "protocol_examples_common.h"

#include "coap3/coap.h"
#include "string.h"
#include <mdns.h>
#include <esp_local_ctrl.h>
#include <esp_timer.h>

#ifndef CONFIG_COAP_SERVER_SUPPORT
#error COAP_SERVER_SUPPORT needs to be enabled
#endif /* COAP_SERVER_SUPPORT */

/* The examples use simple Pre-Shared-Key configuration that you can set via
   'idf.py menuconfig'.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_COAP_PSK_KEY "some-agreed-preshared-key"

   Note: PSK will only be used if the URI is prefixed with coaps://
   instead of coap:// and the PSK must be one that the server supports
   (potentially associated with the IDENTITY)
*/
#define EXAMPLE_COAP_PSK_KEY CONFIG_EXAMPLE_COAP_PSK_KEY

/* The examples use CoAP Logging Level that
   you can set via 'idf.py menuconfig'.

   If you'd rather not, just change the below entry to a value
   that is between 0 and 7 with
   the config you want - ie #define EXAMPLE_COAP_LOG_DEFAULT_LEVEL 7
*/
#define EXAMPLE_COAP_LOG_DEFAULT_LEVEL CONFIG_COAP_LOG_DEFAULT_LEVEL

void xtimer_step_callback(TimerHandle_t pxTimer);

const static char *TAG = "CoAP_server";

//static led_strip_handle_t led_strip;

static char espressif_data[10];
static char g_shoelace_data[6];
static char g_ledcolor_data[8];
static char g_steps_data[50];
static char g_name_data[20]; 
static char g_size_data[20]; 
int    g_steps_int = 0;

static int espressif_data_len = 0;
static int g_shoelace_data_len = 0;
static int g_ledcolor_data_len = 0;
static int g_steps_data_len = 0;
static int g_size_data_len = 0;
static int g_name_data_len = 0;

TimerHandle_t xTimer_Steps;

#ifdef CONFIG_COAP_MBEDTLS_PKI
/* CA cert, taken from coap_ca.pem
   Server cert, taken from coap_server.crt
   Server key, taken from coap_server.key

   The PEM, CRT and KEY file are examples taken from
   https://github.com/eclipse/californium/tree/master/demo-certs/src/main/resources
   as the Certificate test (by default) for the coap_client is against the
   californium server.

   To embed it in the app binary, the PEM, CRT and KEY file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
 */
extern uint8_t ca_pem_start[] asm("_binary_coap_ca_pem_start");
extern uint8_t ca_pem_end[]   asm("_binary_coap_ca_pem_end");
extern uint8_t server_crt_start[] asm("_binary_coap_server_crt_start");
extern uint8_t server_crt_end[]   asm("_binary_coap_server_crt_end");
extern uint8_t server_key_start[] asm("_binary_coap_server_key_start");
extern uint8_t server_key_end[]   asm("_binary_coap_server_key_end");
#endif /* CONFIG_COAP_MBEDTLS_PKI */

#define INITIAL_DATA    "Hello"
#define SHOELACE_TIE    "Tie"
#define SHOELACE_UNTIE  "Untie"

#define LEDCOLOR_BLACK  "000000"
#define LEDCOLOR_WHITE  "FFFFFF"  

#define STEPS_DEFAULT   "000"
#define SIZE_DEFAULT    "20"
#define NAME_DEFAULT    "No name"

//#define SERVICE_NAME "shoe_control._coap._udp.local."
#define SERVICE_NAME "shoe_control._coap._udp.local."

/* Custom allowed property types */
enum property_types {
    PROP_TYPE_TIMESTAMP = 0,
    PROP_TYPE_INT32,
    PROP_TYPE_BOOLEAN,
    PROP_TYPE_STRING,
};

/* Custom flags that can be set for a property */
enum property_flags {
    PROP_FLAG_READONLY = (1 << 0)
};

/********* Handler functions for responding to control requests / commands *********/

static esp_err_t get_property_values(size_t props_count,
                                     const esp_local_ctrl_prop_t props[],
                                     esp_local_ctrl_prop_val_t prop_values[],
                                     void *usr_ctx)
{
    for (uint32_t i = 0; i < props_count; i++) {
        ESP_LOGI(TAG, "Reading property : %s", props[i].name);
        /* For the purpose of this example, to keep things simple
         * we have set the context pointer of each property to
         * point to its value (except for timestamp) */
        switch (props[i].type) {
            case PROP_TYPE_INT32:
            case PROP_TYPE_BOOLEAN:
                /* No need to set size for these types as sizes where
                 * specified when declaring the properties, unlike for
                 * string type. */
                prop_values[i].data = props[i].ctx;
                break;
            case PROP_TYPE_TIMESTAMP: {
                /* Get the time stamp */
                static int64_t ts = 0;
                ts = esp_timer_get_time();

                /* Set the current time. Since this is statically
                 * allocated, we don't need to provide a free_fn */
                prop_values[i].data = &ts;
                break;
            }
            case PROP_TYPE_STRING: {
                char **prop3_value = (char **) props[i].ctx;
                if (*prop3_value == NULL) {
                    prop_values[i].size = 0;
                    prop_values[i].data = NULL;
                } else {
                    /* We could try dynamically allocating the output value,
                     * and it should get freed automatically after use, as
                     * `esp_local_ctrl` internally calls the provided `free_fn` */
                    prop_values[i].size = strlen(*prop3_value);
                    prop_values[i].data = strdup(*prop3_value);
                    if (!prop_values[i].data) {
                        return ESP_ERR_NO_MEM;
                    }
                    prop_values[i].free_fn = free;
                }
            }
            default:
                break;
        }
    }
    return ESP_OK;
}

static esp_err_t set_property_values(size_t props_count,
                                     const esp_local_ctrl_prop_t props[],
                                     const esp_local_ctrl_prop_val_t prop_values[],
                                     void *usr_ctx)
{
    for (uint32_t i = 0; i < props_count; i++) {
        /* Cannot set the value of a read-only property */
        if (props[i].flags & PROP_FLAG_READONLY) {
            ESP_LOGE(TAG, "%s is read-only", props[i].name);
            return ESP_ERR_INVALID_ARG;
        }
        /* For the purpose of this example, to keep things simple
         * we have set the context pointer of each property to
         * point to its value (except for timestamp) */
        switch (props[i].type) {
            case PROP_TYPE_STRING: {
                    /* Free the previously set string */
                    char **prop3_value = (char **) props[i].ctx;
                    free(*prop3_value);
                    *prop3_value = NULL;

                    /* Copy the input string */
                    if (prop_values[i].size) {
                        *prop3_value = strndup((const char *)prop_values[i].data, prop_values[i].size);
                        if (*prop3_value == NULL) {
                            return ESP_ERR_NO_MEM;
                        }
                        ESP_LOGI(TAG, "Setting %s value to %s", props[i].name, (const char*)*prop3_value);
                    }
                }
                break;
            case PROP_TYPE_INT32: {
                    const int32_t *new_value = (const int32_t *) prop_values[i].data;
                    ESP_LOGI(TAG, "Setting %s value to %" PRId32, props[i].name, *new_value);
                    memcpy(props[i].ctx, new_value, sizeof(int32_t));
                }
                break;
            case PROP_TYPE_BOOLEAN: {
                    const bool *value = (const bool *) prop_values[i].data;
                    ESP_LOGI(TAG, "Setting %s value to %d", props[i].name, *value);
                    memcpy(props[i].ctx, value, sizeof(bool));
                }
                break;
            default:
                break;
        }
    }
    return ESP_OK;
}

/******************************************************************************/

/*
 * The resource handler
 */
static void
hnd_espressif_get(coap_resource_t *resource,
                  coap_session_t *session,
                  const coap_pdu_t *request,
                  const coap_string_t *query,
                  coap_pdu_t *response)
{
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
    coap_add_data_large_response(resource, session, request, response,
                                 query, COAP_MEDIATYPE_TEXT_PLAIN, 60, 0,
                                 (size_t)espressif_data_len,
                                 (const u_char *)espressif_data,
                                 NULL, NULL);
}

static void hnd_shoelace_get(coap_resource_t *resource,
                  coap_session_t *session,
                  const coap_pdu_t *request,
                  const coap_string_t *query,
                  coap_pdu_t *response)
{
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
    coap_add_data_large_response(resource, session, request, response,
                                 query, COAP_MEDIATYPE_TEXT_PLAIN, 60, 0,
                                 (size_t)g_shoelace_data_len,
                                 (const u_char *)g_shoelace_data,
                                 NULL, NULL);
    printf("Shoelace get = %s\n", g_shoelace_data);
}

static void hnd_ledcolor_get(coap_resource_t *resource,
                  coap_session_t *session,
                  const coap_pdu_t *request,
                  const coap_string_t *query,
                  coap_pdu_t *response)
{
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
    coap_add_data_large_response(resource, session, request, response,
                                 query, COAP_MEDIATYPE_TEXT_PLAIN, 60, 0,
                                 (size_t)g_ledcolor_data_len,
                                 (const u_char *)g_ledcolor_data,
                                 NULL, NULL);
    printf("LedColor get = %s\n", g_ledcolor_data);
}

static void hnd_steps_get(coap_resource_t *resource,
                  coap_session_t *session,
                  const coap_pdu_t *request,
                  const coap_string_t *query,
                  coap_pdu_t *response)
{
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
    coap_add_data_large_response(resource, session, request, response,
                                 query, COAP_MEDIATYPE_TEXT_PLAIN, 60, 0,
                                 (size_t)g_steps_data_len,
                                 (const u_char *)g_steps_data,
                                 NULL, NULL);
    printf("Steps get = %s\n", g_steps_data);
}

static void hnd_size_get(coap_resource_t *resource,
                  coap_session_t *session,
                  const coap_pdu_t *request,
                  const coap_string_t *query,
                  coap_pdu_t *response)
{
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
    coap_add_data_large_response(resource, session, request, response,
                                 query, COAP_MEDIATYPE_TEXT_PLAIN, 60, 0,
                                 (size_t)g_size_data_len,
                                 (const u_char *)g_size_data,
                                 NULL, NULL);
     printf("Size get = %s\n", g_size_data);
}

static void hnd_name_get(coap_resource_t *resource,
                  coap_session_t *session,
                  const coap_pdu_t *request,
                  const coap_string_t *query,
                  coap_pdu_t *response)
{
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CONTENT);
    coap_add_data_large_response(resource, session, request, response,
                                 query, COAP_MEDIATYPE_TEXT_PLAIN, 60, 0,
                                 (size_t)g_name_data_len,
                                 (const u_char *)g_name_data,
                                 NULL, NULL);
    printf("Name get = %s\n", g_name_data);
}

static void
hnd_espressif_put(coap_resource_t *resource,
                  coap_session_t *session,
                  const coap_pdu_t *request,
                  const coap_string_t *query,
                  coap_pdu_t *response)
{
    size_t size;
    size_t offset;
    size_t total;
    const unsigned char *data;

    coap_resource_notify_observers(resource, NULL);

    if (strcmp (espressif_data, INITIAL_DATA) == 0) {
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_CREATED);
    } else {
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
    }

    /* coap_get_data_large() sets size to 0 on error */
    (void)coap_get_data_large(request, &size, &data, &offset, &total);

    if (size == 0) {      /* re-init */
        snprintf(espressif_data, sizeof(espressif_data), INITIAL_DATA);
        espressif_data_len = strlen(espressif_data);
    } else {
        espressif_data_len = size > sizeof (espressif_data) ? sizeof (espressif_data) : size;
        memcpy (espressif_data, data, espressif_data_len);
    }
}

static void hnd_shoelace_put(coap_resource_t *resource,
                  coap_session_t *session,
                  const coap_pdu_t *request,
                  const coap_string_t *query,
                  coap_pdu_t *response)
{
    size_t size;
    size_t offset;
    size_t total;
    const unsigned char *data;

    coap_resource_notify_observers(resource, NULL);

    if (strcmp (g_shoelace_data, "Untie") == 0) {
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_CREATED);
    } else {
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
    }

    /* coap_get_data_large() sets size to 0 on error */
    (void)coap_get_data_large(request, &size, &data, &offset, &total);

    if (size == 0) {      /* re-init */
        snprintf(g_shoelace_data, sizeof(g_shoelace_data), "Untie");
        g_shoelace_data_len = strlen(g_shoelace_data);
    } else {
        g_shoelace_data_len = size > sizeof (g_shoelace_data) ? sizeof (g_shoelace_data) : size;
        memcpy (g_shoelace_data, data, g_shoelace_data_len);
    }
}

static void hnd_ledcolor_put(coap_resource_t *resource,
                  coap_session_t *session,
                  const coap_pdu_t *request,
                  const coap_string_t *query,
                  coap_pdu_t *response)
{
    size_t size;
    size_t offset;
    size_t total;
    const unsigned char *data;

    coap_resource_notify_observers(resource, NULL);

    if (strcmp (g_ledcolor_data, "000000") == 0) {
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_CREATED);
    } else {
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
    }

    /* coap_get_data_large() sets size to 0 on error */
    (void)coap_get_data_large(request, &size, &data, &offset, &total);

    if (size == 0) {      /* re-init */
        snprintf(g_ledcolor_data, sizeof(g_ledcolor_data), "000000");
        g_ledcolor_data_len = strlen(g_ledcolor_data);
    } else {
        g_ledcolor_data_len = size > sizeof (g_ledcolor_data) ? sizeof (g_ledcolor_data) : size;
        memcpy (g_ledcolor_data, data, g_ledcolor_data_len);
    }
}

static void hnd_name_put(coap_resource_t *resource,
                  coap_session_t *session,
                  const coap_pdu_t *request,
                  const coap_string_t *query,
                  coap_pdu_t *response)
{
    size_t size;
    size_t offset;
    size_t total;
    const unsigned char *data;

    coap_resource_notify_observers(resource, NULL);

    if (strcmp (g_name_data, "No name") == 0) {
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_CREATED);
    } else {
        coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
    }

    /* coap_get_data_large() sets size to 0 on error */
    (void)coap_get_data_large(request, &size, &data, &offset, &total);

    if (size == 0) {      /* re-init */
        snprintf(g_name_data, sizeof(g_name_data), "No name");
        g_name_data_len = strlen(g_name_data);
    } else {
        g_name_data_len = size > sizeof (g_name_data) ? sizeof (g_name_data) : size;
        memcpy (g_name_data, data, g_name_data_len);
    }
}



static void
hnd_espressif_delete(coap_resource_t *resource,
                     coap_session_t *session,
                     const coap_pdu_t *request,
                     const coap_string_t *query,
                     coap_pdu_t *response)
{
    coap_resource_notify_observers(resource, NULL);
    snprintf(espressif_data, sizeof(espressif_data), INITIAL_DATA);
    espressif_data_len = strlen(espressif_data);
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_DELETED);
}

static void hnd_ledcolor_delete(coap_resource_t *resource,
                     coap_session_t *session,
                     const coap_pdu_t *request,
                     const coap_string_t *query,
                     coap_pdu_t *response)
{
    coap_resource_notify_observers(resource, NULL);
    snprintf(g_ledcolor_data, sizeof(g_ledcolor_data), "000000");
    g_ledcolor_data_len = strlen(g_ledcolor_data);
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_DELETED);
}

static void hnd_name_delete(coap_resource_t *resource,
                     coap_session_t *session,
                     const coap_pdu_t *request,
                     const coap_string_t *query,
                     coap_pdu_t *response)
{
    coap_resource_notify_observers(resource, NULL);
    snprintf(g_name_data, sizeof(g_name_data), "No name");
    g_name_data_len = strlen(g_name_data);
    coap_pdu_set_code(response, COAP_RESPONSE_CODE_DELETED);
}

#ifdef CONFIG_COAP_MBEDTLS_PKI

static int
verify_cn_callback(const char *cn,
                   const uint8_t *asn1_public_cert,
                   size_t asn1_length,
                   coap_session_t *session,
                   unsigned depth,
                   int validated,
                   void *arg
                  )
{
    coap_log(LOG_INFO, "CN '%s' presented by server (%s)\n",
             cn, depth ? "CA" : "Certificate");
    return 1;
}
#endif /* CONFIG_COAP_MBEDTLS_PKI */

static void
coap_log_handler (coap_log_t level, const char *message)
{
    uint32_t esp_level = ESP_LOG_INFO;
    char *cp = strchr(message, '\n');

    if (cp)
        ESP_LOG_LEVEL(esp_level, TAG, "%.*s", (int)(cp-message), message);
    else
        ESP_LOG_LEVEL(esp_level, TAG, "%s", message);
}

static void coap_example_server(void *p)
{
    coap_context_t *ctx = NULL;
    coap_address_t serv_addr;
    coap_resource_t *resource = NULL;
    coap_resource_t *resource_shoelace = NULL;
    coap_resource_t *resource_ledcolor = NULL;
    coap_resource_t *resource_steps = NULL;
    coap_resource_t *resource_size = NULL;
    coap_resource_t *resource_name = NULL;
    /**Inits variables*/
    snprintf(espressif_data, sizeof(espressif_data), INITIAL_DATA);
    espressif_data_len = strlen(espressif_data);
    snprintf(g_shoelace_data, sizeof(g_shoelace_data), SHOELACE_UNTIE);
    g_shoelace_data_len = strlen(g_shoelace_data);

    snprintf(g_ledcolor_data, sizeof(g_ledcolor_data), LEDCOLOR_BLACK);
    g_ledcolor_data_len = strlen(g_ledcolor_data);

    /**Converts int to string*/
    snprintf(g_steps_data, sizeof(g_steps_data), STEPS_DEFAULT);
    g_steps_data_len = strlen(g_steps_data);

    snprintf(g_size_data, sizeof(g_size_data), SIZE_DEFAULT);
    g_size_data_len = strlen(g_size_data);

    snprintf(g_name_data, sizeof(g_name_data), NAME_DEFAULT);
    g_name_data_len = strlen(g_name_data);


    /**Inits variables*/
    coap_set_log_handler(coap_log_handler);
    coap_set_log_level(EXAMPLE_COAP_LOG_DEFAULT_LEVEL);
    /**Creates timer for steps*/
    /*Create Timer*/
    xTimer_Steps = xTimerCreate("TimerSteps", 10000 / portTICK_PERIOD_MS, pdTRUE, (void*)0, xtimer_step_callback);
    if(xTimer_Steps == NULL)
    {
    	//ESP_LOGE(xTimer_Steps, "Error Creating Timer\r\n");
    }
    httpd_config_t http_conf = HTTPD_DEFAULT_CONFIG();
    const void *sec_params = NULL;
    esp_local_ctrl_config_t config = {
        .transport = esp_local_ctrl_get_transport_httpd(),
        .transport_config = { .httpd = &http_conf,
        },
        .proto_sec = {
            .version = 0,
            .custom_handle = NULL,
            .sec_params = &sec_params,
        },
        .handlers = {
            /* User defined handler functions */
            .get_prop_values = get_property_values,
            .set_prop_values = set_property_values,
            .usr_ctx         = NULL,
            .usr_ctx_free_fn = NULL
        },
        /* Maximum number of properties that may be set */
        .max_properties = 10
    };    

    mdns_init();
    mdns_hostname_set(SERVICE_NAME);
    /* Start esp_local_ctrl service */
    ESP_ERROR_CHECK(esp_local_ctrl_start(&config));
    ESP_LOGI(TAG, "esp_local_ctrl service started with name : %s", SERVICE_NAME);

    xTimerStart(xTimer_Steps,0);
    while (1) {
        coap_endpoint_t *ep = NULL;
        unsigned wait_ms;
        int have_dtls = 0;

        /* Prepare the CoAP server socket */
        coap_address_init(&serv_addr);
        serv_addr.addr.sin6.sin6_family = AF_INET6;
        serv_addr.addr.sin6.sin6_port   = htons(COAP_DEFAULT_PORT);

        ctx = coap_new_context(NULL);
        if (!ctx) {
            ESP_LOGE(TAG, "coap_new_context() failed");
            continue;
        }
        coap_context_set_block_mode(ctx,
                                    COAP_BLOCK_USE_LIBCOAP|COAP_BLOCK_SINGLE_BODY);
#ifdef CONFIG_COAP_MBEDTLS_PSK
        /* Need PSK setup before we set up endpoints */
        coap_context_set_psk(ctx, "CoAP",
                             (const uint8_t *)EXAMPLE_COAP_PSK_KEY,
                             sizeof(EXAMPLE_COAP_PSK_KEY) - 1);
#endif /* CONFIG_COAP_MBEDTLS_PSK */

#ifdef CONFIG_COAP_MBEDTLS_PKI
        /* Need PKI setup before we set up endpoints */
        unsigned int ca_pem_bytes = ca_pem_end - ca_pem_start;
        unsigned int server_crt_bytes = server_crt_end - server_crt_start;
        unsigned int server_key_bytes = server_key_end - server_key_start;
        coap_dtls_pki_t dtls_pki;

        memset (&dtls_pki, 0, sizeof(dtls_pki));
        dtls_pki.version = COAP_DTLS_PKI_SETUP_VERSION;
        if (ca_pem_bytes) {
            /*
             * Add in additional certificate checking.
             * This list of enabled can be tuned for the specific
             * requirements - see 'man coap_encryption'.
             *
             * Note: A list of root ca file can be setup separately using
             * coap_context_set_pki_root_cas(), but the below is used to
             * define what checking actually takes place.
             */
            dtls_pki.verify_peer_cert        = 1;
            dtls_pki.check_common_ca         = 1;
            dtls_pki.allow_self_signed       = 1;
            dtls_pki.allow_expired_certs     = 1;
            dtls_pki.cert_chain_validation   = 1;
            dtls_pki.cert_chain_verify_depth = 2;
            dtls_pki.check_cert_revocation   = 1;
            dtls_pki.allow_no_crl            = 1;
            dtls_pki.allow_expired_crl       = 1;
            dtls_pki.allow_bad_md_hash       = 1;
            dtls_pki.allow_short_rsa_length  = 1;
            dtls_pki.validate_cn_call_back   = verify_cn_callback;
            dtls_pki.cn_call_back_arg        = NULL;
            dtls_pki.validate_sni_call_back  = NULL;
            dtls_pki.sni_call_back_arg       = NULL;
        }
        dtls_pki.pki_key.key_type = COAP_PKI_KEY_PEM_BUF;
        dtls_pki.pki_key.key.pem_buf.public_cert = server_crt_start;
        dtls_pki.pki_key.key.pem_buf.public_cert_len = server_crt_bytes;
        dtls_pki.pki_key.key.pem_buf.private_key = server_key_start;
        dtls_pki.pki_key.key.pem_buf.private_key_len = server_key_bytes;
        dtls_pki.pki_key.key.pem_buf.ca_cert = ca_pem_start;
        dtls_pki.pki_key.key.pem_buf.ca_cert_len = ca_pem_bytes;

        coap_context_set_pki(ctx, &dtls_pki);
#endif /* CONFIG_COAP_MBEDTLS_PKI */

        ep = coap_new_endpoint(ctx, &serv_addr, COAP_PROTO_UDP);
        if (!ep) {
            ESP_LOGE(TAG, "udp: coap_new_endpoint() failed");
            goto clean_up;
        }
        if (coap_tcp_is_supported()) {
            ep = coap_new_endpoint(ctx, &serv_addr, COAP_PROTO_TCP);
            if (!ep) {
                ESP_LOGE(TAG, "tcp: coap_new_endpoint() failed");
                goto clean_up;
            }
        }
#if defined(CONFIG_COAP_MBEDTLS_PSK) || defined(CONFIG_COAP_MBEDTLS_PKI)
        if (coap_dtls_is_supported()) {
#ifndef CONFIG_MBEDTLS_TLS_SERVER
            /* This is not critical as unencrypted support is still available */
            ESP_LOGI(TAG, "MbedTLS DTLS Server Mode not configured");
#else /* CONFIG_MBEDTLS_TLS_SERVER */
            serv_addr.addr.sin6.sin6_port = htons(COAPS_DEFAULT_PORT);
            ep = coap_new_endpoint(ctx, &serv_addr, COAP_PROTO_DTLS);
            if (!ep) {
                ESP_LOGE(TAG, "dtls: coap_new_endpoint() failed");
                goto clean_up;
            }
            have_dtls = 1;
#endif /* CONFIG_MBEDTLS_TLS_SERVER */
        }
        if (coap_tls_is_supported()) {
#ifndef CONFIG_MBEDTLS_TLS_SERVER
            /* This is not critical as unencrypted support is still available */
            ESP_LOGI(TAG, "MbedTLS TLS Server Mode not configured");
#else /* CONFIG_MBEDTLS_TLS_SERVER */
            serv_addr.addr.sin6.sin6_port = htons(COAPS_DEFAULT_PORT);
            ep = coap_new_endpoint(ctx, &serv_addr, COAP_PROTO_TLS);
            if (!ep) {
                ESP_LOGE(TAG, "tls: coap_new_endpoint() failed");
                goto clean_up;
            }
#endif /* CONFIG_MBEDTLS_TLS_SERVER */
        }
        if (!have_dtls) {
            /* This is not critical as unencrypted support is still available */
            ESP_LOGI(TAG, "MbedTLS (D)TLS Server Mode not configured");
        }
#endif /* CONFIG_COAP_MBEDTLS_PSK || CONFIG_COAP_MBEDTLS_PKI */
        resource = coap_resource_init(coap_make_str_const("Espressif"), 0);
        if (!resource) {
            ESP_LOGE(TAG, "coap_resource_init() failed");
            goto clean_up;
        }
        /**Here we init the resources*/
        resource_shoelace = coap_resource_init(coap_make_str_const("shoe/shoelace"), 0);
        if (!resource) {
            ESP_LOGE(TAG, "coap_resource_init() failed");
            goto clean_up;
        }
        resource_ledcolor = coap_resource_init(coap_make_str_const("shoe/ledcolor"), 0);
        if (!resource) {
            ESP_LOGE(TAG, "coap_resource_init() failed");
            goto clean_up;
        }
        resource_steps = coap_resource_init(coap_make_str_const("shoe/steps"), 0);
        if (!resource) {
            ESP_LOGE(TAG, "coap_resource_init() failed");
            goto clean_up;
        }
        resource_size = coap_resource_init(coap_make_str_const("shoe/size"), 0);
        if (!resource) {
            ESP_LOGE(TAG, "coap_resource_init() failed");
            goto clean_up;
        }
        resource_name = coap_resource_init(coap_make_str_const("shoe/name"), 0);
        if (!resource) {
            ESP_LOGE(TAG, "coap_resource_init() failed");
            goto clean_up;
        }
        /**TODO, create new handlers*/
        coap_register_handler(resource, COAP_REQUEST_GET, hnd_espressif_get);
        coap_register_handler(resource, COAP_REQUEST_PUT, hnd_espressif_put);
        coap_register_handler(resource, COAP_REQUEST_DELETE, hnd_espressif_delete);
        /**Methods for shoelace*/
        coap_register_handler(resource_shoelace, COAP_REQUEST_PUT, hnd_shoelace_put);
        coap_register_handler(resource_shoelace, COAP_REQUEST_GET, hnd_shoelace_get);
        /**Methods for color*/
        coap_register_handler(resource_ledcolor, COAP_REQUEST_PUT, hnd_ledcolor_put);
        coap_register_handler(resource_ledcolor, COAP_REQUEST_GET, hnd_ledcolor_get);
        coap_register_handler(resource_ledcolor, COAP_REQUEST_DELETE, hnd_ledcolor_delete);
        /**Methods for steps*/
        coap_register_handler(resource_steps, COAP_REQUEST_GET, hnd_steps_get);
        /**Methods for size*/
        coap_register_handler(resource_size, COAP_REQUEST_GET, hnd_size_get);
        /**Methods for name*/
        coap_register_handler(resource_name, COAP_REQUEST_PUT, hnd_name_put);
        coap_register_handler(resource_name, COAP_REQUEST_GET, hnd_name_get);
        coap_register_handler(resource_name, COAP_REQUEST_DELETE, hnd_name_delete);

        /* We possibly want to Observe the GETs */
        coap_resource_set_get_observable(resource, 1);
        coap_add_resource(ctx, resource);
        /* We possibly want to Observe the GETs */
        coap_resource_set_get_observable(resource_shoelace, 1);
        coap_add_resource(ctx, resource_shoelace);
        /* We possibly want to Observe the GETs */
        coap_resource_set_get_observable(resource_ledcolor, 1);
        coap_add_resource(ctx, resource_ledcolor);
        /* We possibly want to Observe the GETs */
        coap_resource_set_get_observable(resource_steps, 1);
        coap_add_resource(ctx, resource_steps);
        /* We possibly want to Observe the GETs */
        coap_resource_set_get_observable(resource_size, 1);
        coap_add_resource(ctx, resource_size);
        /* We possibly want to Observe the GETs */
        coap_resource_set_get_observable(resource_name, 1);
        coap_add_resource(ctx, resource_name);

#if defined(CONFIG_EXAMPLE_COAP_MCAST_IPV4) || defined(CONFIG_EXAMPLE_COAP_MCAST_IPV6)
        esp_netif_t *netif = NULL;
        for (int i = 0; i < esp_netif_get_nr_of_ifs(); ++i) {
            char buf[8];
            netif = esp_netif_next(netif);
            esp_netif_get_netif_impl_name(netif, buf);
#if defined(CONFIG_EXAMPLE_COAP_MCAST_IPV4)
            coap_join_mcast_group_intf(ctx, CONFIG_EXAMPLE_COAP_MULTICAST_IPV4_ADDR, buf);
#endif /* CONFIG_EXAMPLE_COAP_MCAST_IPV4 */
#if defined(CONFIG_EXAMPLE_COAP_MCAST_IPV6)
            /* When adding IPV6 esp-idf requires ifname param to be filled in */
            coap_join_mcast_group_intf(ctx, CONFIG_EXAMPLE_COAP_MULTICAST_IPV6_ADDR, buf);
#endif /* CONFIG_EXAMPLE_COAP_MCAST_IPV6 */
        }
#endif /* CONFIG_EXAMPLE_COAP_MCAST_IPV4 || CONFIG_EXAMPLE_COAP_MCAST_IPV6 */

        wait_ms = COAP_RESOURCE_CHECK_TIME * 1000;

        while (1) {
            int result = coap_io_process(ctx, wait_ms);
            if (result < 0) {
                break;
            } else if (result && (unsigned)result < wait_ms) {
                /* decrement if there is a result wait time returned */
                wait_ms -= result;
            }
            if (result) {
                /* result must have been >= wait_ms, so reset wait_ms */
                wait_ms = COAP_RESOURCE_CHECK_TIME * 1000;
            }
        }
    }
clean_up:
    coap_free_context(ctx);
    coap_cleanup();

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK( nvs_flash_init() );
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    xTaskCreate(coap_example_server, "coap", 8 * 1024, NULL, 5, NULL);
}

void xtimer_step_callback(TimerHandle_t pxTimer)
{
    g_steps_int++;
    if(g_steps_int == (0xFFFFFFFF)) //If step counter overflows
    {
       g_steps_int = 0; 
    }
    /**Converts int to string*/
    sprintf(g_steps_data, "%d", g_steps_int);
	//g_steps_data = &g_steps_int;
}
