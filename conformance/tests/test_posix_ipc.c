/**
 * @file test_posix_ipc.c
 * @brief Loopback integration test for reference/c/transport_posix.c
 *        (AF_UNIX framing, EVENT_ACK, burst stream reassembly, and shm_open Plane A ring).
 */

#define _POSIX_C_SOURCE 200809L

#include "onehundo/onehundo.h"
#include "onehundo_internal.h"

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct mock_daemon_args {
    int listen_fd;
    uint32_t events_received;
    uint32_t hello_received;
    uint32_t arena_reg_received;
    uint32_t goodbye_received;
} mock_daemon_args_t;

static bool read_exact(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static void *mock_daemon_thread(void *arg) {
    mock_daemon_args_t *args = (mock_daemon_args_t *)arg;
    int client_fd = accept(args->listen_fd, NULL, NULL);
    assert(client_fd >= 0);

    for (;;) {
        onehundo_wire_header_t hdr;
        char payload[1024];
        if (!read_exact(client_fd, &hdr, sizeof(hdr))) {
            break;
        }
        assert(hdr.magic == ONEHUNDO_WIRE_MAGIC);
        assert(hdr.version == ONEHUNDO_WIRE_VERSION);
        assert(hdr.payload_length < sizeof(payload));
        if (hdr.payload_length > 0U) {
            assert(read_exact(client_fd, payload, hdr.payload_length));
            payload[hdr.payload_length] = '\0';
        } else {
            payload[0] = '\0';
        }

        if (hdr.msg_type == ONEHUNDO_MSG_HELLO) {
            args->hello_received++;
        } else if (hdr.msg_type == ONEHUNDO_MSG_ARENA_REGISTER) {
            args->arena_reg_received++;
        } else if (hdr.msg_type == ONEHUNDO_MSG_EVENT_PUBLISH) {
            onehundo_wire_header_t ack;
            char ack_json[128];
            int ack_len;
            args->events_received++;
            ack_len = snprintf(
                ack_json,
                sizeof(ack_json),
                "{\"status\":\"ok\",\"sequence_number\":%llu,\"deduplicated\":false}",
                (unsigned long long)hdr.sequence_number
            );
            assert(ack_len > 0 && (size_t)ack_len < sizeof(ack_json));
            memset(&ack, 0, sizeof(ack));
            ack.magic = ONEHUNDO_WIRE_MAGIC;
            ack.version = ONEHUNDO_WIRE_VERSION;
            ack.msg_type = ONEHUNDO_MSG_EVENT_ACK;
            ack.payload_length = (uint32_t)ack_len;
            ack.sequence_number = hdr.sequence_number;
            assert(write(client_fd, &ack, sizeof(ack)) == (ssize_t)sizeof(ack));
            assert(write(client_fd, ack_json, (size_t)ack_len) == (ssize_t)ack_len);
        } else if (hdr.msg_type == ONEHUNDO_MSG_GOODBYE) {
            args->goodbye_received++;
            break;
        }
    }

    close(client_fd);
    return NULL;
}


static void test_xdg_runtime_socket_discovery(void) {
    const char *orig_sock = getenv("ONEHUNDO_SOCKET");
    const char *orig_xdg = getenv("XDG_RUNTIME_DIR");
    char saved_sock[256];
    char saved_xdg[256];
    int has_orig_sock = 0;
    int has_orig_xdg = 0;
    char tmpdir[64];
    char xdg_sock[108];
    int listen_fd;
    struct sockaddr_un addr;
    pthread_t daemon_tid;
    mock_daemon_args_t dargs;
    onehundo_client_config_t cfg;
    onehundo_context_t *ctx = NULL;
    onehundo_diagnostics_t diag;
    int tries;

    if (orig_sock) {
        snprintf(saved_sock, sizeof(saved_sock), "%s", orig_sock);
        has_orig_sock = 1;
    }
    if (orig_xdg) {
        snprintf(saved_xdg, sizeof(saved_xdg), "%s", orig_xdg);
        has_orig_xdg = 1;
    }

    snprintf(tmpdir, sizeof(tmpdir), "/tmp/oh_xdg_%ld", (long)getpid());
    assert(mkdir(tmpdir, 0700) == 0);
    assert(snprintf(xdg_sock, sizeof(xdg_sock), "%s/onehundo.sock", tmpdir) < (int)sizeof(xdg_sock));
    unlink(xdg_sock);

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listen_fd >= 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, xdg_sock, sizeof(addr.sun_path) - 1U);
    assert(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listen_fd, 4) == 0);

    memset(&dargs, 0, sizeof(dargs));
    dargs.listen_fd = listen_fd;
    assert(pthread_create(&daemon_tid, NULL, mock_daemon_thread, &dargs) == 0);

    unsetenv("ONEHUNDO_SOCKET");
    setenv("XDG_RUNTIME_DIR", tmpdir, 1);

    onehundo_client_config_init(&cfg);
    cfg.canonical_game_id = "sony.bloodborne";
    cfg.producer_id = "bbport_xdg_test";
    cfg.ipc_endpoint_path = "";

    assert(onehundo_init(&ctx, &cfg, sizeof(cfg)) == ONEHUNDO_OK);
    assert(onehundo_unlock_request(ctx, "a01") == ONEHUNDO_OK);

    for (tries = 0; tries < 100; ++tries) {
        memset(&diag, 0, sizeof(diag));
        diag.struct_size = sizeof(diag);
        assert(onehundo_get_diagnostics(ctx, &diag, sizeof(diag)) == ONEHUNDO_OK);
        if (diag.events_acked == 1U) {
            break;
        }
        {
            struct timespec ts = {0, 5000000};
            nanosleep(&ts, NULL);
        }
    }

    assert(diag.is_connected == 1U);
    assert(diag.events_acked == 1U);

    onehundo_shutdown(ctx);
    assert(pthread_join(daemon_tid, NULL) == 0);
    close(listen_fd);
    unlink(xdg_sock);
    rmdir(tmpdir);

    assert(dargs.hello_received == 1U);
    assert(dargs.events_received == 1U);
    assert(dargs.goodbye_received == 1U);

    if (has_orig_sock) {
        setenv("ONEHUNDO_SOCKET", saved_sock, 1);
    } else {
        unsetenv("ONEHUNDO_SOCKET");
    }
    if (has_orig_xdg) {
        setenv("XDG_RUNTIME_DIR", saved_xdg, 1);
    } else {
        unsetenv("XDG_RUNTIME_DIR");
    }
}

int main(void) {
    char sock_path[108];
    int listen_fd;
    struct sockaddr_un addr;
    pthread_t daemon_tid;
    mock_daemon_args_t dargs;
    onehundo_client_config_t cfg;
    onehundo_context_t *ctx = NULL;
    onehundo_region_descriptor_t region;
    onehundo_diagnostics_t diag;
    uint32_t slot_idx = 0U;
    void *slot_buf = NULL;
    size_t slot_cap = 0U;
    onehundo_snapshot_slot_header_t shdr;
    int i;
    int tries;

    snprintf(sock_path, sizeof(sock_path), "/tmp/onehundo_test_%ld.sock", (long)getpid());
    unlink(sock_path);

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listen_fd >= 0);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1U);
    assert(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(listen_fd, 4) == 0);

    memset(&dargs, 0, sizeof(dargs));
    dargs.listen_fd = listen_fd;
    assert(pthread_create(&daemon_tid, NULL, mock_daemon_thread, &dargs) == 0);

    onehundo_client_config_init(&cfg);
    cfg.canonical_game_id = "sony.bloodborne";
    cfg.producer_id = "bbport_posix_test";
    cfg.ingestion_plane = ONEHUNDO_PLANE_MEMORY_ARENA;
    cfg.ipc_endpoint_path = sock_path;

    assert(onehundo_init(&ctx, &cfg, sizeof(cfg)) == ONEHUNDO_OK);

    memset(&region, 0, sizeof(region));
    region.struct_size = sizeof(region);
    region.api_version = ONEHUNDO_API_VERSION;
    region.region_id = 0U;
    region.guest_base_address = 0x10000000ULL;
    region.size_bytes = 2048U;
    region.endianness = ONEHUNDO_ENDIAN_LITTLE;
    region.coherence = ONEHUNDO_COHERENCE_COHERENT;
    region.region_name = "TestRAM";
    assert(onehundo_arena_register_region(ctx, &region, sizeof(region)) == ONEHUNDO_OK);

    assert(onehundo_arena_acquire_slot(ctx, &slot_idx, &slot_buf, &slot_cap) == ONEHUNDO_OK);
    memset(slot_buf, 0x42, 32U);
    memset(&shdr, 0, sizeof(shdr));
    shdr.struct_size = sizeof(shdr);
    shdr.api_version = ONEHUNDO_API_VERSION;
    shdr.sequence_number = 1U;
    shdr.lifecycle_signal = ONEHUNDO_SIGNAL_NORMAL_FRAME;
    shdr.payload_size_bytes = 32U;
    assert(onehundo_arena_publish_slot(ctx, slot_idx, &shdr) == ONEHUNDO_OK);

    assert(onehundo_stat_inc(ctx, "bosses_defeated", 1) == ONEHUNDO_OK);
    assert(onehundo_unlock_request(ctx, "ach_cleric_beast") == ONEHUNDO_OK);
    for (i = 0; i < 58; ++i) {
        char id[16];
        snprintf(id, sizeof(id), "a%02d", (i % 40) + 1);
        assert(onehundo_unlock_request(ctx, id) == ONEHUNDO_OK);
    }

    for (tries = 0; tries < 200; ++tries) {
        memset(&diag, 0, sizeof(diag));
        diag.struct_size = sizeof(diag);
        assert(onehundo_get_diagnostics(ctx, &diag, sizeof(diag)) == ONEHUNDO_OK);
        if (diag.events_acked == 60U) {
            break;
        }
        {
            struct timespec ts = {0, 5000000};
            nanosleep(&ts, NULL);
        }
    }

    assert(diag.is_connected == 1U);
    assert(diag.events_emitted == 60U);
    assert(diag.events_acked == 60U);
    assert(diag.frame_snapshots_published == 1U);

    onehundo_shutdown(ctx);
    assert(pthread_join(daemon_tid, NULL) == 0);
    close(listen_fd);
    unlink(sock_path);

    assert(dargs.hello_received == 1U);
    assert(dargs.arena_reg_received == 1U);
    assert(dargs.events_received == 60U);
    assert(dargs.goodbye_received == 1U);

    test_xdg_runtime_socket_discovery();
    printf("POSIX IPC & shm_open test PASSED.\n");
    return 0;
}
