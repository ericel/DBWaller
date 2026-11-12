#include <catch2/catch_session.hpp>
#include <dbwaller/remote/client.hpp>

int main(int argc, char** argv) {
    dbwaller::remote::initialize_process_tls();
    return Catch::Session().run(argc, argv);
}
