#include "ps3emu/coop_net.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static uint64_t clock_ms(void) { return GetTickCount64(); }
static void wait_a_bit(void) { Sleep(10); }
static void set_mode(const char *mode) { _putenv_s("INFAMOUS_COOP_MODE", mode); }
#else
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
static uint64_t clock_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}
static void wait_a_bit(void) { usleep(10000); }
static void set_mode(const char *mode) { setenv("INFAMOUS_COOP_MODE", mode, 1); }
#endif

int main(int argc, char **argv)
{
    ps3emu_coop_player_state local = {0};
    ps3emu_coop_remote_state remote;
    uint64_t deadline;
    float expected;

    if (argc != 2 || (strcmp(argv[1], "host") && strcmp(argv[1], "client"))) {
        fprintf(stderr, "usage: %s host|client\n", argv[0]);
        return 2;
    }
    set_mode(argv[1]);
    local.position[0] = strcmp(argv[1], "host") == 0 ? 1.0f : 2.0f;
    expected = local.position[0] == 1.0f ? 2.0f : 1.0f;
    if (!ps3emu_coop_init()) {
        fprintf(stderr, "init failed: %s\n", ps3emu_coop_last_error());
        return 1;
    }

    deadline = clock_ms() + 5000;
    while (clock_ms() < deadline) {
        ps3emu_coop_tick(clock_ms());
        ps3emu_coop_send_local_state(&local);
        if (ps3emu_coop_get_remote_state(&remote) &&
            remote.player.position[0] == expected) {
            printf("%s received peer state\n", argv[1]);
            ps3emu_coop_shutdown();
            return 0;
        }
        wait_a_bit();
    }
    {
        ps3emu_coop_status status;
        ps3emu_coop_get_status(&status);
        fprintf(stderr, "%s timed out (enabled=%d mode=%d connected=%d rejected=%u error=%s)\n",
                argv[1], status.enabled, (int)status.mode, status.connected, status.rejected_packets,
                ps3emu_coop_last_error());
    }
    ps3emu_coop_shutdown();
    return 1;
}
