#include <stdatomic.h>
#include <signal.h>
#include <stdbool.h>

#include "pumpnet/lib/prinet.h"
#include "pumpnet/prinet-proxy/options.h"
#include "pumpnet/prinet-proxy/packet.h"
#include "sec/prinet/prinet.h"

#include "util/iobuf.h"
#include "util/log.h"
#include "util/mem.h"
#include "util/sock-tcp.h"

static const uint32_t NUM_CONNECTIONS = 10;
static const size_t PUMPNET_MAX_RESP_SIZE = 1024 * 1024;

static atomic_int _source_sock;

/* Compiled binary data from data folder. Symbols are defined by compiler */
extern const uint8_t _binary_prime_private_key_start[];
extern const uint8_t _binary_prime_private_key_end[];
extern const uint8_t _binary_prime_public_key_start[];
extern const uint8_t _binary_prime_public_key_end[];

static bool _transform_data_request(const struct pumpnet_prinet_proxy_packet* packet, struct util_iobuf* dec_data)
{
    size_t enc_data_len = pumpnet_prinet_proxy_packet_get_data_len(packet);

    size_t dec_data_len = sec_prinet_decrypt(
        packet->nounce,
        sizeof(packet->nounce),
        packet->data,
        enc_data_len,
        dec_data->bytes);

    if (dec_data_len != enc_data_len) {
        log_error("Decrypting data failed");
        return false;
    }

    dec_data->pos = dec_data_len;

    return true;
}

static bool _transform_data_response(const struct pumpnet_prinet_proxy_packet* req_packet, const struct util_iobuf* dec_data, struct pumpnet_prinet_proxy_packet* resp_packet)
{
    size_t enc_data_len = sec_prinet_encrypt(
        req_packet->nounce,
        sizeof(req_packet->nounce),
        dec_data->bytes,
        dec_data->pos,
        resp_packet->data);

    if (enc_data_len < 0) {
        log_error("Encrypting data failed");
        return false;
    }

    memcpy(resp_packet->nounce, req_packet->nounce, sizeof(resp_packet->nounce));

    resp_packet->length = pumpnet_prinet_proxy_packet_get_header_len() + enc_data_len;

    return true;
}

static bool _recv_packet_length(int handle, uint32_t* length)
{
    uint32_t packet_length;

    while (true) {
        ssize_t read = util_sock_tcp_recv_block(handle, &packet_length, sizeof(uint32_t));

        if (read == 0) {
            log_error("Unexpected remote close and no data");
            return NULL;
        }

        if (read != sizeof(uint32_t)) {
            return false;
        }

        break;
    }

    *length = packet_length;

    return true;
}

static struct pumpnet_prinet_proxy_packet* _recv_request_source(int handle)
{
    uint32_t packet_length;

    if (!_recv_packet_length(handle, &packet_length)) {
        log_error("Receiving length field for request from source failed");
        return NULL;
    }

    log_debug("Received length source request (%X): %d", handle, packet_length);

    struct pumpnet_prinet_proxy_packet* packet = util_xmalloc(packet_length);
    packet->length = packet_length;

    size_t data_len = pumpnet_prinet_proxy_packet_get_data_len(packet);

    size_t read_pos = 0;

    while (true) {
        size_t remaining_size = packet->length - sizeof(uint32_t) - read_pos;

        ssize_t read = util_sock_tcp_recv_block(
            handle,
            ((uint8_t*) packet) + sizeof(uint32_t) + read_pos,
            remaining_size);

        if (read == 0) {
            log_error("Unexpected remote close and no data");
            return NULL;
        }

        if (read == -1) {
            log_error("Receiving length field for request from source failed: %d", read);
            return NULL;
        }

        read_pos += read;

        if (read_pos >= data_len) {
            break;
        }
    }

    return packet;
}

static bool _send_response_source(int handle, const struct pumpnet_prinet_proxy_packet* packet)
{
    log_debug("Sending source response (%X): %d", handle, packet->length);

    if (util_sock_tcp_send_block(handle, packet, packet->length) != packet->length) {
        log_error("Sending response, len %d, to source failed", packet->length);
        return false;
    } else {
        return true;
    }
}

static void _sigint_handler(int sig)
{
    log_info("SIGINT, exiting");

    util_sock_tcp_close(_source_sock);

    sec_prinet_finit();

    pumpnet_lib_prinet_shutdown();

    exit(EXIT_SUCCESS);
}

int main(int argc, char** argv)
{
    util_log_set_level(LOG_LEVEL_DEBUG);

    struct pumpnet_prinet_proxy_options options;

    if (!pumpnet_prinet_proxy_options_init(argc, argv, &options)) {
        return -1;
    }

    util_log_set_file(options.log.file, false);
    util_log_set_level(options.log.level);

    pumpnet_lib_prinet_init(
        options.proxy.pumpnet.game,
        options.proxy.pumpnet.server,
        options.proxy.pumpnet.machine_id,
        options.proxy.pumpnet.cert_dir_path,
        options.proxy.pumpnet.verbose_log_output);

    log_info("Proxy, source (listen/sink) %s:%d -> pumpnet server %s (game: %d)",
        options.proxy.source.addr,
        options.proxy.source.port,
        options.proxy.pumpnet.server,
        options.proxy.pumpnet.game);

    sec_prinet_init(
        (const uint8_t*) _binary_prime_public_key_start,
        ((uintptr_t) &_binary_prime_public_key_end -
        (uintptr_t) &_binary_prime_public_key_start),
        (const uint8_t*) _binary_prime_private_key_start,
        ((uintptr_t) &_binary_prime_private_key_end -
        (uintptr_t) &_binary_prime_private_key_start));

    _source_sock = util_sock_tcp_open_bind_listen2(
        options.proxy.source.addr,
        options.proxy.source.port,
        NUM_CONNECTIONS);

    if (_source_sock == INVALID_SOCK_HANDLE) {
        return -3;
    }

    log_info("Running proxy loop");

    signal(SIGINT, _sigint_handler);

    while (true) {
        int source_con = INVALID_SOCK_HANDLE;
        struct pumpnet_prinet_proxy_packet* source_req = NULL;
        struct util_iobuf pumpnet_data_req;
        struct util_iobuf pumpnet_data_resp;
        struct pumpnet_prinet_proxy_packet* source_resp = NULL;

        source_con = util_sock_tcp_wait_and_accept_remote_connection(_source_sock);

        log_debug("Received connection: %X", source_con);

        if (source_con == INVALID_SOCK_HANDLE) {
            log_error("Waiting and accepting source connection failed");
            goto cleanup_iteration;
        }

        source_req = _recv_request_source(source_con);

        if (!source_req) {
            log_error("Receiving request from source failed");
            goto cleanup_iteration;
        }

        size_t source_data_len = pumpnet_prinet_proxy_packet_get_data_len(source_req);
        util_iobuf_alloc(&pumpnet_data_req, source_data_len);

        if (!_transform_data_request(source_req, &pumpnet_data_req)) {
            log_error("Transforming data request for destination failed");
            goto cleanup_iteration;
        }

        util_iobuf_alloc(&pumpnet_data_resp, PUMPNET_MAX_RESP_SIZE);

        ssize_t pumpnet_recv_size = pumpnet_lib_prinet_msg(
            pumpnet_data_req.bytes,
            pumpnet_data_req.pos,
            pumpnet_data_resp.bytes,
            pumpnet_data_resp.nbytes);

        if (pumpnet_recv_size < 0) {
            log_error("Request to pumpnet failed");
            goto cleanup_iteration;
        }

        // Communicate actual data size
        pumpnet_data_resp.pos = pumpnet_recv_size;

        source_resp = pumpnet_prinet_proxy_packet_alloc_response(pumpnet_data_resp.pos);

        if (!_transform_data_response(source_req, &pumpnet_data_resp, source_resp)) {
            log_error("Transforming data response for source failed");
            goto cleanup_iteration;
        }

        if (!_send_response_source(source_con, source_resp)) {
            log_error("Sending response to source failed");
            goto cleanup_iteration;
        }

    cleanup_iteration:
        if (source_con != INVALID_SOCK_HANDLE) {
            util_sock_tcp_close(source_con);
        }

        if (source_req != NULL) {
            util_xfree((void**) &source_req);
        }

        if (pumpnet_data_req.bytes != NULL) {
            util_iobuf_free(&pumpnet_data_req);
        }

        if (pumpnet_data_resp.bytes != NULL) {
            util_iobuf_free(&pumpnet_data_resp);
        }

        if (source_resp != NULL) {
            util_xfree((void**) &source_resp);
        }
    }
}