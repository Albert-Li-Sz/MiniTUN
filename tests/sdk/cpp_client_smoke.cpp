#include <minitun/client.hpp>

int main() {
    auto client = minitun::Client::create("/tmp/minitun-sdk-smoke-does-not-exist.sock");
    if (!client) {
        return 1;
    }
    auto identity = client.value().identity();
    if (identity) {
        return 2;
    }
    minitun::ServerUpdate update;
    update.identifier = "missing";
    update.name = minitun::UpdateField<std::string>::set("renamed");
    return update.name.specified ? 0 : 3;
}
