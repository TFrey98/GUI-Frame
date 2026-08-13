/*
 * Checkpoint: "good/bad configs produce the right messages." Exercises
 * every check listener_config_validate performs, including the two that
 * need existing registry state to trigger (name/endpoint uniqueness).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "listeners/listener_manager.h"
#include "listeners/object_registry.h"

static ListenerConfig make_config(const char *name, ListenerType type, const char *bind_address, uint16_t port,
                                   const char *callback_host, const char *cert_path, const char *key_path) {
    ListenerConfig config = {0};
    config.name = name ? strdup(name) : NULL;
    config.type = type;
    config.bind_address = bind_address ? strdup(bind_address) : NULL;
    config.port = port;
    config.callback_host = callback_host ? strdup(callback_host) : NULL;
    config.cert_path = cert_path ? strdup(cert_path) : NULL;
    config.key_path = key_path ? strdup(key_path) : NULL;
    return config;
}

static void free_config(ListenerConfig *config) {
    free(config->name);
    free(config->bind_address);
    free(config->callback_host);
    free(config->cert_path);
    free(config->key_path);
    free(config->url_path);
    free(config->host_header);
}

static bool has_field(const ListenerConfigValidation *validation, ListenerConfigField field) {
    for (int i = 0; i < validation->error_count; i++) {
        if (validation->errors[i].field == field) {
            return true;
        }
    }
    return false;
}

int main(void) {
    int status = 0;
    ObjectRegistry *registry = object_registry_create();

    {
        ListenerConfig config =
            make_config("Valid", LISTENER_TYPE_REVERSE_TCP, "0.0.0.0", 4444, "203.0.113.1", NULL, NULL);
        ListenerConfigValidation validation;
        bool ok = listener_config_validate(registry, &config, &validation);
        if (!ok || validation.error_count != 0) {
            fprintf(stderr, "listener_config_validate_test: expected a fully valid config to pass, got %d errors\n",
                    validation.error_count);
            status = 1;
        }
        free_config(&config);
    }

    {
        /* All three real ListenerTypes are valid as of the HTTPS
         * provider phase - an out-of-range value stands in for
         * "unsupported type" to still exercise that check. */
        ListenerConfig config = make_config("", (ListenerType)99, "not-an-ip", 0, "0.0.0.0", NULL, NULL);
        ListenerConfigValidation validation;
        bool ok = listener_config_validate(registry, &config, &validation);
        if (ok) {
            fprintf(stderr, "listener_config_validate_test: expected an all-wrong config to fail\n");
            status = 1;
        }
        static const ListenerConfigField expected[] = {
            LISTENER_CONFIG_FIELD_NAME, LISTENER_CONFIG_FIELD_TYPE, LISTENER_CONFIG_FIELD_PORT,
            LISTENER_CONFIG_FIELD_BIND_ADDRESS, LISTENER_CONFIG_FIELD_CALLBACK_HOST,
        };
        for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
            if (!has_field(&validation, expected[i])) {
                fprintf(stderr, "listener_config_validate_test: expected field %d to be flagged, was not\n",
                        expected[i]);
                status = 1;
            }
        }
        free_config(&config);
    }

    {
        ListenerConfig config =
            make_config("Https", LISTENER_TYPE_HTTPS, "0.0.0.0", 8443, "203.0.113.1", NULL, NULL);
        ListenerConfigValidation validation;
        listener_config_validate(registry, &config, &validation);
        if (!has_field(&validation, LISTENER_CONFIG_FIELD_CERT_PATH)) {
            fprintf(stderr, "listener_config_validate_test: expected missing cert_path to be flagged\n");
            status = 1;
        }
        if (!has_field(&validation, LISTENER_CONFIG_FIELD_KEY_PATH)) {
            fprintf(stderr, "listener_config_validate_test: expected missing key_path to be flagged\n");
            status = 1;
        }
        free_config(&config);
    }

    {
        /* url_path/host_header aren't in make_config()'s signature (only
         * cert_path/key_path are, from when HTTPS validation was built
         * ahead of the provider itself) - set directly. */
        ListenerConfig config = make_config("Http", LISTENER_TYPE_HTTP, "0.0.0.0", 8080, "203.0.113.1", NULL, NULL);
        ListenerConfigValidation validation;
        listener_config_validate(registry, &config, &validation);
        if (!has_field(&validation, LISTENER_CONFIG_FIELD_URL_PATH)) {
            fprintf(stderr, "listener_config_validate_test: expected missing url_path to be flagged\n");
            status = 1;
        }
        free_config(&config);
    }

    {
        ListenerConfig config = make_config("Http2", LISTENER_TYPE_HTTP, "0.0.0.0", 8081, "203.0.113.1", NULL, NULL);
        config.url_path = strdup("no-leading-slash");
        ListenerConfigValidation validation;
        listener_config_validate(registry, &config, &validation);
        if (!has_field(&validation, LISTENER_CONFIG_FIELD_URL_PATH)) {
            fprintf(stderr, "listener_config_validate_test: expected a url_path without a leading / to be flagged\n");
            status = 1;
        }
        free_config(&config);
    }

    {
        /* host_header left blank (unset) - optional, should not be flagged. */
        ListenerConfig config = make_config("Http3", LISTENER_TYPE_HTTP, "0.0.0.0", 8082, "203.0.113.1", NULL, NULL);
        config.url_path = strdup("/checkin");
        ListenerConfigValidation validation;
        bool ok = listener_config_validate(registry, &config, &validation);
        if (!ok) {
            fprintf(stderr,
                    "listener_config_validate_test: expected a valid HTTP config with a blank host_header to pass\n");
            status = 1;
        }
        free_config(&config);
    }

    {
        ListenerConfig config = make_config("V6", LISTENER_TYPE_REVERSE_TCP, "::1", 4444, "203.0.113.1", NULL, NULL);
        ListenerConfigValidation validation;
        listener_config_validate(registry, &config, &validation);
        if (has_field(&validation, LISTENER_CONFIG_FIELD_BIND_ADDRESS)) {
            fprintf(stderr, "listener_config_validate_test: expected '::1' to be accepted as a valid bind address\n");
            status = 1;
        }
        free_config(&config);
    }

    {
        ListenerConfig existing =
            make_config("Existing", LISTENER_TYPE_REVERSE_TCP, "0.0.0.0", 5555, "203.0.113.1", NULL, NULL);
        uint64_t id = object_registry_add_listener(registry, existing); /* takes ownership of existing's fields */
        if (id == 0) {
            fprintf(stderr, "listener_config_validate_test: failed to seed an existing listener\n");
            status = 1;
        }

        ListenerConfig dup_name =
            make_config("Existing", LISTENER_TYPE_REVERSE_TCP, "0.0.0.0", 6666, "203.0.113.1", NULL, NULL);
        ListenerConfigValidation name_validation;
        listener_config_validate(registry, &dup_name, &name_validation);
        if (!has_field(&name_validation, LISTENER_CONFIG_FIELD_NAME)) {
            fprintf(stderr, "listener_config_validate_test: expected a reused name to be flagged\n");
            status = 1;
        }
        free_config(&dup_name);

        ListenerConfig dup_endpoint =
            make_config("Different", LISTENER_TYPE_REVERSE_TCP, "0.0.0.0", 5555, "203.0.113.1", NULL, NULL);
        ListenerConfigValidation endpoint_validation;
        listener_config_validate(registry, &dup_endpoint, &endpoint_validation);
        if (!has_field(&endpoint_validation, LISTENER_CONFIG_FIELD_ENDPOINT)) {
            fprintf(stderr, "listener_config_validate_test: expected a reused bind_address:port to be flagged\n");
            status = 1;
        }
        free_config(&dup_endpoint);
    }

    object_registry_destroy(registry);

    if (status == 0) {
        printf("listener_config_validate_test: field-level and uniqueness validation verified\n");
    }
    return status;
}
