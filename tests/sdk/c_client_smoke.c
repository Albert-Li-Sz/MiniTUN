#include <minitun/client.h>

#include <stddef.h>

int main(void) {
    if (minitun_client_abi_version() != MINITUN_CLIENT_ABI_VERSION) {
        return 1;
    }
    const minitun_client_options options = {
        .struct_size = sizeof(minitun_client_options),
        .socket_path = "/tmp/minitun-sdk-smoke-does-not-exist.sock",
    };
    minitun_client* client = NULL;
    minitun_error* error = NULL;
    if (minitun_client_create(&options, &client, &error) != 0 || client == NULL || error != NULL) {
        minitun_error_free(error);
        return 2;
    }
    minitun_status status = {0};
    if (minitun_client_status_get(client, &status, &error) == 0 || error == NULL) {
        minitun_error_free(error);
        minitun_client_destroy(client);
        return 3;
    }
    minitun_error_free(error);
    minitun_client_destroy(client);
    return 0;
}
