#include <grpcpp/grpcpp.h>
#include <grpcpp/generic/callback_generic_service.h>
#include <openssl/sha.h>
#include <fstream>
#include <iterator>
#include <chrono>
std::string read(const char* path) { std::ifstream in(path); return {std::istreambuf_iterator<char>(in), {}}; }
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    unsigned char digest[SHA256_DIGEST_LENGTH]; SHA256(reinterpret_cast<const unsigned char*>("probe"), 5, digest);
    for (int i = 0; i != 10; ++i) {
        grpc::CallbackGenericService service;
        grpc::ServerBuilder builder;
        grpc::SslServerCredentialsOptions tls; tls.pem_key_cert_pairs.push_back({read(argv[2]), read(argv[1])});
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::SslServerCredentials(tls), &port);
        builder.RegisterCallbackGenericService(&service);
        auto server = builder.BuildAndStart(); if (!server || !port) return 3;
        grpc::SslCredentialsOptions roots; roots.pem_root_certs = read(argv[1]);
        auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::SslCredentials(roots));
        if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(10))) return 4;
        channel.reset(); server->Shutdown(); server->Wait(); server.reset();
    }
}
