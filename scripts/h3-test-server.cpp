// Minimal HTTP/3 test origin for the experimental QUIC CI job.
//
// It serves "/" with "hello-h3" and every other path with "path=<path>".
// A self-signed certificate is generated in memory, so clients must disable
// verification. This helper intentionally handles one QUIC connection and is
// not part of the shipped daemon.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <uvcpp/uvcpp_define.h>

#if !UVCPP_HTTP3_ENABLE
int main() {
  std::fprintf(stderr, "this build has no HTTP/3 support\n");
  return 2;
}
#else

#include <handle/uvcpp_loop.h>
#include <http3/uvcpp_h3_common.h>
#include <http3/uvcpp_h3_connection.h>
#include <quic/uvcpp_quic_connection.h>
#include <quic/uvcpp_quic_server.h>
#include <ssl/uvcpp_ssl_context.h>

namespace {

const char* body_for(const std::string& path) {
  return path == "/" ? "hello-h3" : nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  int port = 0;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--host") == 0) {
      host = argv[++i];
    } else if (std::strcmp(argv[i], "--port") == 0) {
      port = std::atoi(argv[++i]);
    }
  }

  uvcpp::uvcpp_loop loop;
  uvcpp::uvcpp_ssl_context ssl(uvcpp::tls_mode::SERVER, uvcpp::tls_version::TLS_1_3);
  if (!ssl.generate_self_signed("localhost", 2048)) {
    std::fprintf(stderr, "could not generate the test certificate\n");
    return 3;
  }

  uvcpp::uvcpp_quic_server server(&loop);
  server.set_ssl_context(&ssl);
  server.set_alpn_select_protos({"h3"});
  if (server.bind(host.c_str(), port) != 0) {
    std::fprintf(stderr, "could not bind %s:%d\n", host.c_str(), port);
    return 4;
  }

  std::unique_ptr<uvcpp::uvcpp_h3_connection> h3;
  const int listen_rc = server.listen([&h3](uvcpp::uvcpp_quic_connection* conn) {
    if (h3) return;  // Test origin: one connection is enough.
    h3.reset(new uvcpp::uvcpp_h3_connection(conn, true));
    uvcpp::uvcpp_h3_connection::callbacks callbacks;
    callbacks.on_request = [](uvcpp::uvcpp_h3_connection& connection,
                              const uvcpp::h3_request& request) {
      uvcpp::h3_response response;
      response.stream_id = request.stream_id;
      response.status = 200;
      uvcpp::h3_header content_type;
      content_type.name = "content-type";
      content_type.value = "text/plain";
      response.headers.push_back(content_type);
      if (const char* body = body_for(request.path)) {
        response.body = body;
      } else {
        response.body = "path=" + request.path;
      }
      connection.send_response(response);
    };
    callbacks.on_error = [](uvcpp::uvcpp_h3_connection&, int code) {
      std::fprintf(stderr, "h3 error: %d\n", code);
    };
    h3->start(callbacks);
  });
  if (listen_rc != 0) {
    std::fprintf(stderr, "could not listen: %d\n", listen_rc);
    return 5;
  }

  std::printf("H3 LISTENING %d\n", server.configured_port());
  std::fflush(stdout);
  server.run(UV_RUN_DEFAULT);
  return 0;
}

#endif
