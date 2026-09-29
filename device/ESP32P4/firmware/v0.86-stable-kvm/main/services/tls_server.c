#include "tls_server.h"
#include <stdio.h>
#include <string.h>
#include "esp_https_server.h"
#include "esp_random.h"
#include "mbedtls/ecp.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "net_manager.h"
#include "secret_store.h"
#include "settings_store.h"
#include "lwip/inet.h"

/* Private key stays on the device. The public certificate is delivered over UART0. */
static unsigned char s_certificate[2048];
static unsigned char s_private_key[1024];
static bool s_ready;

static int random_bytes(void *ctx, unsigned char *out, size_t size)
{
    (void)ctx;
    esp_fill_random(out, size);
    return 0;
}

static esp_err_t generate_identity(void)
{
    mbedtls_pk_context key;
    mbedtls_x509write_cert certificate;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&certificate);
    int ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (ret == 0) ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                                            mbedtls_pk_ec(key), random_bytes, NULL);
    uint8_t serial_bytes[16];
    esp_fill_random(serial_bytes, sizeof(serial_bytes));
    serial_bytes[0] &= 0x7f;
    serial_bytes[0] |= 1;
    if (ret == 0) ret = mbedtls_x509write_crt_set_serial_raw(&certificate, serial_bytes, sizeof(serial_bytes));
    mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&certificate, &key);
    mbedtls_x509write_crt_set_issuer_key(&certificate, &key);
    si_net_status_t net;
    si_net_get_status(&net);
    char hostname[64], subject[96];
    char mac_hex[13] = {0};
    size_t n = 0;
    for (size_t i = 0; net.mac[i] && n < sizeof(mac_hex) - 1; ++i)
        if (net.mac[i] != ':') mac_hex[n++] = net.mac[i];
    if (n != 12) { ret = -1; }
    snprintf(hostname, sizeof(hostname), "exoanchor-%s.local", mac_hex);
    snprintf(subject, sizeof(subject), "CN=%s,O=ExoAnchor", hostname);
    if (ret == 0) ret = mbedtls_x509write_crt_set_subject_name(&certificate, subject);
    if (ret == 0) ret = mbedtls_x509write_crt_set_issuer_name(&certificate, subject);
    if (ret == 0) ret = mbedtls_x509write_crt_set_validity(&certificate,
                                                        "20250101000000", "20450101000000");
    if (ret == 0) ret = mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1);
    if (ret == 0) ret = mbedtls_x509write_crt_set_key_usage(&certificate, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    /* DER GeneralNames with a stable DNS identity and the initial IPv4 address. */
    uint8_t san[80];
    size_t host_size = strlen(hostname), san_size = 0;
    san[san_size++] = 0x30;
    san[san_size++] = (uint8_t)(host_size + 2);
    san[san_size++] = 0x82;
    san[san_size++] = (uint8_t)host_size;
    memcpy(san + san_size, hostname, host_size);
    san_size += host_size;
    struct in_addr address;
    if (inet_aton(net.ip, &address) == 1) {
        san[1] += 6;
        san[san_size++] = 0x87;
        san[san_size++] = 4;
        memcpy(san + san_size, &address.s_addr, 4);
        san_size += 4;
    }
    if (ret == 0) ret = mbedtls_x509write_crt_set_extension(&certificate,
        MBEDTLS_OID_SUBJECT_ALT_NAME, MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),
        0, san, san_size);
    if (ret == 0) ret = mbedtls_pk_write_key_pem(&key, s_private_key, sizeof(s_private_key));
    if (ret == 0) ret = mbedtls_x509write_crt_pem(&certificate, s_certificate,
        sizeof(s_certificate), random_bytes, NULL);
    mbedtls_x509write_crt_free(&certificate);
    mbedtls_pk_free(&key);
    si_secret_store_clear(serial_bytes, sizeof(serial_bytes));
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t load_identity(void)
{
    if (s_ready) return ESP_OK;
    si_settings_store_t store = SI_SETTINGS_STORE_INITIALIZER;
    esp_err_t ret = si_settings_store_open_read(&store, "si_tls");
    if (ret == ESP_OK) {
        ret = si_settings_store_get_string(&store, "certificate", (char *)s_certificate, sizeof(s_certificate));
        esp_err_t key_ret = si_secret_store_get_string(&store, "private_key", (char *)s_private_key, sizeof(s_private_key));
        si_settings_store_close(&store);
        /* Partial/corrupt records require local recovery; never downgrade to HTTP. */
        if (ret != ESP_OK || key_ret != ESP_OK) return ESP_FAIL;
    } else if (ret == ESP_ERR_NOT_FOUND) {
        ret = generate_identity();
        if (ret == ESP_OK) ret = si_settings_store_open_write(&store, "si_tls");
        if (ret == ESP_OK) ret = si_settings_store_set_string(&store, "certificate", (char *)s_certificate);
        if (ret == ESP_OK) ret = si_secret_store_set_string(&store, "private_key", (char *)s_private_key);
        if (ret == ESP_OK) ret = si_settings_store_commit(&store);
        si_settings_store_close(&store);
        if (ret != ESP_OK) {
            si_secret_store_clear(s_private_key, sizeof(s_private_key));
            return ret;
        }
    } else return ret;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_x509_crt_init(&cert);
    mbedtls_pk_init(&key);
    int parsed = mbedtls_x509_crt_parse(&cert, s_certificate, strlen((char *)s_certificate) + 1);
    if (!parsed) parsed = mbedtls_pk_parse_key(&key, s_private_key,
        strlen((char *)s_private_key) + 1, NULL, 0, random_bytes, NULL);
    if (!parsed) parsed = mbedtls_pk_check_pair(&cert.pk, &key, random_bytes, NULL);
    if (!parsed) {
        uint8_t digest[32];
        parsed = mbedtls_sha256(cert.raw.p, cert.raw.len, digest, 0);
        if (!parsed) {
            printf("ExoAnchor TLS certificate SHA256: ");
            for (size_t i = 0; i < sizeof(digest); ++i) printf("%02x", digest[i]);
            printf("\n%s", s_certificate);
        }
    }
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&cert);
    if (parsed) {
        si_secret_store_clear(s_private_key, sizeof(s_private_key));
        return ESP_FAIL;
    }
    s_ready = true;
    return ESP_OK;
}

esp_err_t si_tls_server_start(httpd_handle_t *server, const httpd_config_t *config)
{
    if (!server || !config) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = load_identity();
    if (ret != ESP_OK) return ret;
    httpd_ssl_config_t tls = HTTPD_SSL_CONFIG_DEFAULT();
    tls.httpd = *config;
    tls.httpd.stack_size = config->stack_size < 8192 ? 8192 : config->stack_size;
    tls.port_secure = config->server_port;
    tls.servercert = s_certificate;
    tls.servercert_len = strlen((char *)s_certificate) + 1;
    tls.prvtkey_pem = s_private_key;
    tls.prvtkey_len = strlen((char *)s_private_key) + 1;
    tls.tls_handshake_timeout_ms = 5000;
    return httpd_ssl_start(server, &tls);
}
