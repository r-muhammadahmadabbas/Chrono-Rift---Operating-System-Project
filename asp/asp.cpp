#include "../arbiter/shared_state.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

static Config g_cfg;
static GameState *g_state = NULL;
static int g_shmid = -1;
static int g_running = 1;
static int g_ecount = 0;

static pthread_t g_threads[9];

static inline void asp_state_enter(int me)
{
    bakery3_enter(&g_state->lock, me);
}

static inline void asp_state_exit(int me)
{
    bakery3_exit(&g_state->lock, me);
}

static void handle_stun(int sig)
{
    (void)sig;
    struct timespec ts;
    ts.tv_sec = 3;
    ts.tv_nsec = 0;
    nanosleep(&ts, NULL);
}

static void handle_sigterm(int sig)
{
    (void)sig;
    g_running = 0;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void shm_open_existing()
{
    int retries = 50;
    while (retries-- > 0) {
        g_shmid = shmget(g_cfg.shm_key, sizeof(GameState), 0666);
        if (g_shmid >= 0) break;
        sleep_ms(100);
    }
    if (g_shmid < 0) {
        perror("ASP: shmget");
        exit(1);
    }

    g_state = (GameState *)shmat(g_shmid, NULL, 0);
    if (g_state == (GameState *)-1) {
        perror("ASP: shmat");
        exit(1);
    }

    if (g_state->version != g_cfg.shm_version) {
        fprintf(stderr, "ASP: SHM version mismatch (got %d, want %d)\n",
                g_state->version, g_cfg.shm_version);
        exit(1);
    }

    printf("[ASP] Connected to shared memory. shmid=%d\n", g_shmid);

    asp_state_enter(g_cfg.proc_asp);
    g_state->asp_pid = getpid();
    asp_state_exit(g_cfg.proc_asp);
}

static void wait_for_playing()
{
    printf("[ASP] Waiting for game start...\n");
    while (g_running) {
        asp_state_enter(g_cfg.proc_asp);
        int ph = g_state->phase;
        asp_state_exit(g_cfg.proc_asp);

        if (ph == g_cfg.phase_playing) break;
        sleep_ms(100);
    }
    printf("[ASP] Game started.\n");
}

static int choose_target_player_random(unsigned int *seed)
{
    int alive_count = 0;
    for (int i = 0; i < g_state->player_count; i++) {
        if (g_state->players[i].alive) alive_count++;
    }
    if (alive_count <= 0) return 0;

    int pick = (int)(rand_r(seed) % (unsigned int)alive_count);
    for (int i = 0; i < g_state->player_count; i++) {
        if (!g_state->players[i].alive) continue;
        if (pick-- == 0) return i;
    }
    return 0;
}

static void *npc_thread(void *arg)
{
    int idx = (int)(long)arg;
    unsigned int seed = (unsigned int)g_cfg.roll_seed ^ (unsigned int)(0x9e3779b9u * (unsigned int)(idx + 1));

    printf("[ASP] NPC thread %d started.\n", idx);

    while (g_running) {
        asp_state_enter(g_cfg.proc_asp);
        int ph = g_state->phase;
        int alive = (idx < g_state->enemy_count) ? g_state->enemies[idx].alive : 0;
        int go = g_state->asp_go;
        int act_is_player = g_state->active_is_player;
        int act_idx = g_state->active_index;
        asp_state_exit(g_cfg.proc_asp);

        if (ph != g_cfg.phase_playing) break;

        if (!alive) {
            sleep_ms(50);
            continue;
        }

        if (!go || act_is_player || act_idx != idx) {
            sleep_ms(20);
            continue;
        }

        asp_state_enter(g_cfg.proc_asp);

        int action = g_cfg.enemy_skip;
        int target = 0;

        int any_alive = 0;
        for (int i = 0; i < g_state->player_count; i++) {
            if (g_state->players[i].alive) {
                any_alive = 1;
                break;
            }
        }

        if (any_alive) {
            action = g_cfg.enemy_strike;
            target = choose_target_player_random(&seed);
        }

        g_state->asp_buf.action = action;
        g_state->asp_buf.attacker_index = idx;
        g_state->asp_buf.target_index = target;
        g_state->asp_buf.ready = 1;

        asp_state_exit(g_cfg.proc_asp);

        while (g_running) {
            asp_state_enter(g_cfg.proc_asp);
            int still = g_state->asp_go;
            int ph2 = g_state->phase;
            asp_state_exit(g_cfg.proc_asp);

            if (!still || ph2 != g_cfg.phase_playing) break;
            sleep_ms(20);
        }
    }

    printf("[ASP] NPC thread %d exiting.\n", idx);
    return NULL;
}

int main()
{
    load_config(&g_cfg, "config.txt");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stun;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    signal(SIGTERM, handle_sigterm);

    shm_open_existing();
    wait_for_playing();

    if (!g_running) {
        shmdt(g_state);
        return 0;
    }

    asp_state_enter(g_cfg.proc_asp);
    g_ecount = g_state->enemy_count;
    asp_state_exit(g_cfg.proc_asp);

    if (g_ecount < 0) g_ecount = 0;
    if (g_ecount > 9) g_ecount = 9;

    printf("[ASP] Spawning %d NPC thread(s).\n", g_ecount);
    srand(g_cfg.roll_seed);

    for (int i = 0; i < g_ecount; i++)
        pthread_create(&g_threads[i], NULL, npc_thread, (void *)(long)i);

    while (g_running) {
        asp_state_enter(g_cfg.proc_asp);
        int ph = g_state->phase;
        asp_state_exit(g_cfg.proc_asp);

        if (ph == g_cfg.phase_gameover || ph == g_cfg.phase_win) break;
        sleep_ms(100);
    }

    g_running = 0;

    for (int i = 0; i < g_ecount; i++)
        pthread_join(g_threads[i], NULL);

    shmdt(g_state);
    printf("[ASP] Clean shutdown.\n");
    return 0;
}
