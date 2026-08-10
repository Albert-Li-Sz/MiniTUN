#ifndef MINITUN_CLIENT_H
#define MINITUN_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(MINITUN_CLIENT_BUILD)
#define MINITUN_CLIENT_API __declspec(dllexport)
#else
#define MINITUN_CLIENT_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define MINITUN_CLIENT_API __attribute__((visibility("default")))
#else
#define MINITUN_CLIENT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MINITUN_CLIENT_ABI_VERSION 1u

typedef struct minitun_client minitun_client;

typedef enum minitun_error_code {
    MINITUN_ERROR_OK = 0,
    MINITUN_ERROR_INVALID_ARGUMENT = 1,
    MINITUN_ERROR_NOT_FOUND = 2,
    MINITUN_ERROR_ALREADY_EXISTS = 3,
    MINITUN_ERROR_PERMISSION_DENIED = 4,
    MINITUN_ERROR_NOT_AUTHENTICATED = 5,
    MINITUN_ERROR_AUTHENTICATION_FAILED = 6,
    MINITUN_ERROR_CONNECTION_FAILED = 7,
    MINITUN_ERROR_CONNECTION_TIMEOUT = 8,
    MINITUN_ERROR_REMOTE_PORT_IN_USE = 9,
    MINITUN_ERROR_LOCAL_CONNECT_FAILED = 10,
    MINITUN_ERROR_PROTOCOL = 11,
    MINITUN_ERROR_RESOURCE_EXHAUSTED = 12,
    MINITUN_ERROR_DATABASE = 13,
    MINITUN_ERROR_TLS = 14,
    MINITUN_ERROR_IPC = 15,
    MINITUN_ERROR_UNSUPPORTED_VERSION = 16,
    MINITUN_ERROR_INTERNAL = 17
} minitun_error_code;

typedef struct minitun_error {
    minitun_error_code code;
    char* message;
} minitun_error;

typedef struct minitun_client_options {
    uint32_t struct_size;
    const char* socket_path;
} minitun_client_options;

typedef struct minitun_identity {
    char* client_id;
} minitun_identity;

typedef struct minitun_status {
    uint64_t server_total;
    uint64_t server_online;
    uint64_t tunnel_total;
    uint64_t tunnel_active;
    uint64_t sessions_active;
    uint64_t workers_idle;
    uint64_t workers_active;
    uint64_t connections_active;
} minitun_status;

typedef struct minitun_server_info {
    char* id;
    char* name;
    char* endpoint;
    char* tls_server_name;
    char* desired_state;
    char* actual_state;
    uint64_t config_revision;
    uint8_t credential_configured;
    uint8_t ca_configured;
    uint8_t client_certificate_configured;
    uint8_t managed_by_config;
} minitun_server_info;

typedef struct minitun_server_list {
    minitun_server_info* items;
    size_t size;
} minitun_server_list;

typedef struct minitun_server_create_request {
    uint32_t struct_size;
    const char* endpoint;
    const char* name;
} minitun_server_create_request;

enum {
    MINITUN_SERVER_UPDATE_NAME = 1u << 0,
    MINITUN_SERVER_UPDATE_ENDPOINT = 1u << 1,
    MINITUN_SERVER_UPDATE_TLS_SERVER_NAME = 1u << 2,
    MINITUN_SERVER_UPDATE_CA_FILE = 1u << 3,
    MINITUN_SERVER_UPDATE_CLIENT_CERT_FILE = 1u << 4,
    MINITUN_SERVER_UPDATE_CLIENT_KEY_FILE = 1u << 5
};

typedef struct minitun_server_update_request {
    uint32_t struct_size;
    const char* identifier;
    uint32_t field_mask;
    const char* name;
    const char* endpoint;
    const char* tls_server_name;
    const char* ca_file;
    const char* client_cert_file;
    const char* client_key_file;
} minitun_server_update_request;

typedef enum minitun_server_action {
    MINITUN_SERVER_ENABLE = 1,
    MINITUN_SERVER_DISABLE = 2,
    MINITUN_SERVER_LOGOUT = 3,
    MINITUN_SERVER_REMOVE = 4
} minitun_server_action;

typedef struct minitun_tunnel_info {
    char* id;
    char* name;
    char* server_id;
    char* local_endpoint;
    char* remote_endpoint;
    char* desired_state;
    char* actual_state;
    uint64_t config_revision;
    uint8_t managed_by_config;
} minitun_tunnel_info;

typedef struct minitun_tunnel_list {
    minitun_tunnel_info* items;
    size_t size;
} minitun_tunnel_list;

typedef struct minitun_tunnel_create_request {
    uint32_t struct_size;
    const char* server;
    const char* name;
    const char* local_host;
    uint16_t local_port;
    uint16_t remote_port;
} minitun_tunnel_create_request;

enum {
    MINITUN_TUNNEL_UPDATE_NAME = 1u << 0,
    MINITUN_TUNNEL_UPDATE_LOCAL_HOST = 1u << 1,
    MINITUN_TUNNEL_UPDATE_LOCAL_PORT = 1u << 2,
    MINITUN_TUNNEL_UPDATE_REMOTE_PORT = 1u << 3
};

typedef struct minitun_tunnel_update_request {
    uint32_t struct_size;
    const char* identifier;
    uint32_t field_mask;
    const char* name;
    const char* local_host;
    uint16_t local_port;
    uint16_t remote_port;
} minitun_tunnel_update_request;

typedef enum minitun_tunnel_action {
    MINITUN_TUNNEL_ENABLE = 1,
    MINITUN_TUNNEL_DISABLE = 2,
    MINITUN_TUNNEL_REMOVE = 3
} minitun_tunnel_action;

typedef enum minitun_config_action_kind {
    MINITUN_CONFIG_CREATE = 1,
    MINITUN_CONFIG_UPDATE = 2,
    MINITUN_CONFIG_DISABLE = 3,
    MINITUN_CONFIG_DELETE = 4
} minitun_config_action_kind;

typedef enum minitun_config_resource_kind {
    MINITUN_CONFIG_SERVER = 1,
    MINITUN_CONFIG_TUNNEL = 2
} minitun_config_resource_kind;

typedef struct minitun_config_action_info {
    minitun_config_action_kind action;
    minitun_config_resource_kind resource;
    char* id;
    char* name;
} minitun_config_action_info;

typedef struct minitun_config_plan_result {
    minitun_config_action_info* actions;
    size_t size;
    uint8_t prune;
} minitun_config_plan_result;

typedef struct minitun_config_snapshot {
    minitun_server_list servers;
    minitun_tunnel_list tunnels;
} minitun_config_snapshot;

typedef struct minitun_diagnostics {
    uint8_t healthy;
    uint8_t ready;
    uint8_t state_database_ok;
    uint8_t credential_database_ok;
} minitun_diagnostics;

MINITUN_CLIENT_API uint32_t minitun_client_abi_version(void);
MINITUN_CLIENT_API int minitun_client_create(const minitun_client_options* options,
                                             minitun_client** output,
                                             minitun_error** error);
MINITUN_CLIENT_API void minitun_client_destroy(minitun_client* client);
MINITUN_CLIENT_API void minitun_error_free(minitun_error* error);

MINITUN_CLIENT_API int minitun_client_identity_get(minitun_client* client,
                                                   minitun_identity* output,
                                                   minitun_error** error);
MINITUN_CLIENT_API void minitun_identity_free(minitun_identity* value);
MINITUN_CLIENT_API int minitun_client_status_get(minitun_client* client,
                                                 minitun_status* output,
                                                 minitun_error** error);

MINITUN_CLIENT_API int minitun_client_server_create(
    minitun_client* client, const minitun_server_create_request* request,
    minitun_server_info* output, minitun_error** error);
MINITUN_CLIENT_API int minitun_client_server_login(minitun_client* client,
                                                   const char* identifier,
                                                   const char* psk,
                                                   minitun_server_info* output,
                                                   minitun_error** error);
MINITUN_CLIENT_API int minitun_client_server_update(
    minitun_client* client, const minitun_server_update_request* request,
    minitun_server_info* output, minitun_error** error);
MINITUN_CLIENT_API int minitun_client_server_execute(minitun_client* client,
                                                     const char* identifier,
                                                     minitun_server_action action,
                                                     minitun_server_info* output,
                                                     minitun_error** error);
MINITUN_CLIENT_API int minitun_client_server_list(minitun_client* client,
                                                  minitun_server_list* output,
                                                  minitun_error** error);
MINITUN_CLIENT_API void minitun_server_info_free(minitun_server_info* value);
MINITUN_CLIENT_API void minitun_server_list_free(minitun_server_list* value);

MINITUN_CLIENT_API int minitun_client_tunnel_create(
    minitun_client* client, const minitun_tunnel_create_request* request,
    minitun_tunnel_info* output, minitun_error** error);
MINITUN_CLIENT_API int minitun_client_tunnel_update(
    minitun_client* client, const minitun_tunnel_update_request* request,
    minitun_tunnel_info* output, minitun_error** error);
MINITUN_CLIENT_API int minitun_client_tunnel_execute(minitun_client* client,
                                                     const char* identifier,
                                                     minitun_tunnel_action action,
                                                     minitun_tunnel_info* output,
                                                     minitun_error** error);
MINITUN_CLIENT_API int minitun_client_tunnel_list(minitun_client* client,
                                                  const char* server,
                                                  minitun_tunnel_list* output,
                                                  minitun_error** error);
MINITUN_CLIENT_API void minitun_tunnel_info_free(minitun_tunnel_info* value);
MINITUN_CLIENT_API void minitun_tunnel_list_free(minitun_tunnel_list* value);

MINITUN_CLIENT_API int minitun_client_config_plan(minitun_client* client,
                                                  const char* path,
                                                  uint8_t prune,
                                                  minitun_config_plan_result* output,
                                                  minitun_error** error);
MINITUN_CLIENT_API int minitun_client_config_apply(minitun_client* client,
                                                   const char* path,
                                                   uint8_t prune,
                                                   minitun_config_plan_result* output,
                                                   minitun_error** error);
MINITUN_CLIENT_API int minitun_client_config_export(minitun_client* client,
                                                    minitun_config_snapshot* output,
                                                    minitun_error** error);
MINITUN_CLIENT_API void minitun_config_plan_result_free(minitun_config_plan_result* value);
MINITUN_CLIENT_API void minitun_config_snapshot_free(minitun_config_snapshot* value);

MINITUN_CLIENT_API int minitun_client_diagnostics_get(minitun_client* client,
                                                      minitun_diagnostics* output,
                                                      minitun_error** error);

#ifdef __cplusplus
}
#endif

#endif
