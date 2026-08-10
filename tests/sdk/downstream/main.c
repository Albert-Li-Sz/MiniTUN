#include <minitun/client.h>

int main(void) {
    return minitun_client_abi_version() == MINITUN_CLIENT_ABI_VERSION ? 0 : 1;
}
