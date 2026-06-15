
#include "../arbiter/shared_state.h"


#define KEY_A     0
#define KEY_E     4
#define KEY_H     7
#define KEY_I     8
#define KEY_Q     16
#define KEY_S     18
#define KEY_W     22
#define KEY_P     15
#define KEY_L     11
#define KEY_U     20
#define KEY_NUM1  27
#define KEY_NUM9  35

static int weapon_id_from_key(int key)
{
    if (key < KEY_NUM1 || key > KEY_NUM9) return 0;
    int n = (key - KEY_NUM1) + 1;
    if (n < 1 || n > 8) return 0;
    return n;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <pthread.h>
#include <time.h>

typedef struct {
    int buf[64];
    int head;
    int tail;
} InputQueue;

static Config g_cfg;
static GameState *g_state = NULL;
static int g_shmid = -1;
static int g_running = 1;
static int g_pcount = 0;

static pthread_t g_threads[4];  
static InputQueue g_input;

static inline int inq_push(InputQueue *q, int key);
static inline int inq_pop(InputQueue *q, int *out);

static inline void hip_state_enter(int me)
{
    bakery3_enter(&g_state->lock, me);
}

static inline void hip_state_exit(int me)
{
    bakery3_exit(&g_state->lock, me);
}

static inline int hip_input_pop(int *out)
{
    int ok;
    ok = inq_pop(&g_input, out);
    return ok;
}

static inline void hip_input_push(int key)
{
    (void)inq_push(&g_input, key);
}

static inline void inq_init(InputQueue *q)
{
    q->head = 0;
    q->tail = 0;
    for (int i = 0; i < 64; i++) q->buf[i] = -1;
}

static inline int inq_push(InputQueue *q, int key)
{
    int next = (q->tail + 1) % 64;
    if (next == q->head) return 0; // full, drop
    q->buf[q->tail] = key;
    q->tail = next;
    return 1;
}

static inline int inq_pop(InputQueue *q, int *out)
{
    if (q->head == q->tail) return 0;
    *out = q->buf[q->head];
    q->buf[q->head] = -1;
    q->head = (q->head + 1) % 64;
    return 1;
}

static void handle_stun(int sig)
{
    (void)sig;
    struct timespec ts;
    ts.tv_sec = 3;
    ts.tv_nsec = 0;
    nanosleep(&ts, NULL);
}

static void shm_open_existing()
{
    int retries = 40;
    while (retries-- > 0) {
        g_shmid = shmget(g_cfg.shm_key, sizeof(GameState), 0666);
        if (g_shmid >= 0) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    if (g_shmid < 0) { perror("HIP: shmget"); exit(1); }

    g_state = (GameState *) shmat(g_shmid, NULL, 0);
    if (g_state == (GameState *) -1) { perror("HIP: shmat"); exit(1); }

    if (g_state->version != g_cfg.shm_version) {
        fprintf(stderr, "HIP: SHM version mismatch (got %d, want %d)\n",
                g_state->version, g_cfg.shm_version);
        exit(1);
    }
    printf("[HIP] Connected to shared memory. shmid=%d\n", g_shmid);

    // Register PID so Arbiter can signal this process (stun, etc.)
    hip_state_enter(g_cfg.proc_hip);
    g_state->hip_pid = getpid();
    hip_state_exit(g_cfg.proc_hip);
}

static void wait_for_playing()
{
    printf("[HIP] Waiting for party selection to complete...\n");
    while (g_running) {
        hip_state_enter(g_cfg.proc_hip);
        int ph = g_state->phase;
        hip_state_exit(g_cfg.proc_hip);
        if (ph == g_cfg.phase_playing) break;
        struct timespec ts = {0, 100000000};
        nanosleep(&ts, NULL);
    }
    printf("[HIP] Game started.\n");
}

static int read_key_for_turn(int my_idx)
{
    while (g_running) {
        int k = -1;
        if (hip_input_pop(&k)) return k;

        // Abort if no longer our turn.
        hip_state_enter(g_cfg.proc_hip);
        int ph = g_state->phase;
        int go = g_state->hip_go;
        int act_is_pl = g_state->active_is_player;
        int act_idx = g_state->active_index;
        hip_state_exit(g_cfg.proc_hip);

        if (ph != g_cfg.phase_playing) return -1;
        if (!go || !act_is_pl || act_idx != my_idx) return -1;

        struct timespec ts = {0, 20000000};
        nanosleep(&ts, NULL);
    }
    return -1;
}

static void *player_thread(void *arg)
{
    int idx = (int)(long)arg;

    // Single shared target selection for the whole party.
    // Only the active player's thread will modify it (by design), so plain int is sufficient.
    static int g_selected_enemy = -1;

    auto pick_first_alive_enemy = [&](void) -> int {
        int ecount = 0;
        Entity enemies[9];
        hip_state_enter(g_cfg.proc_hip);
        ecount = g_state->enemy_count;
        for (int i = 0; i < 9; i++) enemies[i] = g_state->enemies[i];
        hip_state_exit(g_cfg.proc_hip);

        if (ecount < 0) ecount = 0;
        if (ecount > 9) ecount = 9;
        for (int i = 0; i < ecount; i++) {
            if (enemies[i].alive) return i;
        }
        return -1;
    };

    auto fix_target = [&](int t) -> int {
        int ecount = 0;
        Entity enemies[9];
        hip_state_enter(g_cfg.proc_hip);
        ecount = g_state->enemy_count;
        for (int i = 0; i < 9; i++) enemies[i] = g_state->enemies[i];
        hip_state_exit(g_cfg.proc_hip);

        if (ecount < 0) ecount = 0;
        if (ecount > 9) ecount = 9;

        if (t >= 0 && t < ecount && enemies[t].alive) return t;
        for (int i = 0; i < ecount; i++) {
            if (enemies[i].alive) return i;
        }
        return -1;
    };

    printf("[HIP] Player thread %d started (%s).\n",
           idx, g_state->players[idx].name);

    while (g_running)
    {
        hip_state_enter(g_cfg.proc_hip);
        int go = g_state->hip_go;
        int act_idx = g_state->active_index;
        int act_is_pl = g_state->active_is_player;
        int ph = g_state->phase;
        int drop_active = g_state->drop.active;
        int drop_for = g_state->drop.for_player;
        int ready = g_state->hip_buf.ready;
        hip_state_exit(g_cfg.proc_hip);

        if (ph != g_cfg.phase_playing) break;

        if (!go || !act_is_pl || act_idx != idx || ready) {
            struct timespec ts = {0, 50000000};
            nanosleep(&ts, NULL);
            continue;
        }

        // It is this player's turn — either handle drop prompt or normal actions

        int action = g_cfg.action_skip;
        int target = 0;
        int aux = 0;

        if (drop_active && drop_for == idx) {
            printf("[HIP] %s: weapon drop! (P=Pick up, L=Leave)\n",
                   g_state->players[idx].name);
            int key = -1;
            while (g_running) {
                key = read_key_for_turn(idx);
                if (key >= 0) {
                    printf("[HIP DEBUG] Key pressed: %d\n", key);
                }
                if (key < 0) {
                    // Check if turn ended due to timeout
                    hip_state_enter(g_cfg.proc_hip);
                    int still_go = g_state->hip_go;
                    hip_state_exit(g_cfg.proc_hip);
                    if (!still_go) {
                        printf("[HIP DEBUG] Timeout! Auto-skipping drop.\n");
                        break;
                    }
                    continue; 
                }
                if (key == KEY_P || key == KEY_L) {
                    printf("[HIP DEBUG] Valid key %s accepted!\n", (key == KEY_P) ? "P" : "L");
                    break;
                } else {
                    printf("[HIP DEBUG] Ignored invalid key. Please press P or L.\n");
                }
            }
            if (!g_running) break;
            if (key < 0) continue; // aborted by timeout
            aux = (key == KEY_P) ? 1 : 0;

            // action value is ignored by Arbiter during drop prompt; aux decides.
            action = g_cfg.action_none;
        } else {
            // Ensure we have a reasonable default target.
            if (g_selected_enemy < 0) g_selected_enemy = pick_first_alive_enemy();
            g_selected_enemy = fix_target((int)g_selected_enemy);

            printf("[HIP] %s: 1-9=Select Target  A=Strike  E=Exhaust  H=Heal  W=Weapon  I=SwapIn  U=Ultimate  S=Skip  Q=Quit\n",
                   g_state->players[idx].name);

            // Read keys until a turn-consuming action is chosen.
            while (g_running) {
                int key = read_key_for_turn(idx);
                if (!g_running) break;
                if (key < 0) { action = g_cfg.action_skip; break; }

                // Target selection does NOT consume the turn.
                if (key >= KEY_NUM1 && key <= KEY_NUM9) {
                    int t = (key - KEY_NUM1);
                    hip_state_enter(g_cfg.proc_hip);
                    int ecount = g_state->enemy_count;
                    hip_state_exit(g_cfg.proc_hip);
                    if (t >= 0 && t < ecount) {
                        g_selected_enemy = t;
                        // Publish selection for HUD highlight (does NOT submit a turn).
                        hip_state_enter(g_cfg.proc_hip);
                        int still_go = g_state->hip_go;
                        int still_act_is_pl = g_state->active_is_player;
                        int still_act_idx = g_state->active_index;
                        if (still_go && still_act_is_pl && still_act_idx == idx) {
                            g_state->hip_buf.target_index = (int)g_selected_enemy;
                        }
                        hip_state_exit(g_cfg.proc_hip);
                        printf("[HIP] %s selected enemy %d\n", g_state->players[idx].name, t + 1);
                    }
                    continue;
                }

                if (key == KEY_A) {
                    action = g_cfg.action_strike;
                    int ft = fix_target((int)g_selected_enemy);
                    if (ft < 0) { action = g_cfg.action_skip; target = 0; }
                    else { g_selected_enemy = ft; target = ft; }
                    break;
                }
                if (key == KEY_E) {
                    action = g_cfg.action_exhaust;
                    int ft = fix_target((int)g_selected_enemy);
                    if (ft < 0) { action = g_cfg.action_skip; target = 0; }
                    else { g_selected_enemy = ft; target = ft; }
                    break;
                }
                if (key == KEY_H) { action = g_cfg.action_heal; break; }
                if (key == KEY_S) { action = g_cfg.action_skip; break; }
                if (key == KEY_U) { action = g_cfg.action_ultimate; break; }
                if (key == KEY_Q) {
                    // Spec quit condition: HIP must send SIGTERM to Arbiter.
                    int arb = 0;
                    hip_state_enter(g_cfg.proc_hip);
                    arb = g_state->arbiter_pid;
                    hip_state_exit(g_cfg.proc_hip);
                    if (arb > 0) kill(arb, SIGTERM);

                    // Also submit quit action for backward compatibility.
                    action = g_cfg.action_quit;
                    break;
                }

                if (key == KEY_W) {
                    // Weapon ID consumes one extra key (still same turn).
                    int wk = read_key_for_turn(idx);
                    if (wk < 0) continue;
                    int wid = weapon_id_from_key(wk);
                    if (wid <= 0) continue;

                    action = g_cfg.action_use_weapon;
                    aux = wid;
                    int ft = fix_target((int)g_selected_enemy);
                    if (ft < 0) { action = g_cfg.action_skip; target = 0; aux = 0; }
                    else { g_selected_enemy = ft; target = ft; }
                    break;
                }

                if (key == KEY_I) {
                    int wk = read_key_for_turn(idx);
                    if (wk < 0) continue;
                    int wid = weapon_id_from_key(wk);
                    if (wid <= 0) continue;
                    action = g_cfg.action_swap_in;
                    aux = wid;
                    break;
                }

                // Unknown key: ignore and keep waiting.
            }
        }

        // Commit action to shared buffer (only if still our turn)
        hip_state_enter(g_cfg.proc_hip);
        int still_go = g_state->hip_go;
        int still_act_is_pl = g_state->active_is_player;
        int still_act_idx = g_state->active_index;
        if (still_go && still_act_is_pl && still_act_idx == idx) {
            g_state->hip_buf.action = action;
            g_state->hip_buf.target_index = target;
            g_state->hip_buf.aux = aux;
            g_state->hip_buf.ready = 1;
        }
        hip_state_exit(g_cfg.proc_hip);

         printf("[HIP] %s submitted action=%d target=%d aux=%d\n",
             g_state->players[idx].name, action, target, aux);
    }

    printf("[HIP] Player thread %d exiting.\n", idx);
    return NULL;
}

static void handle_sigterm(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(int argc, char *argv[])
{
    load_config(&g_cfg, "config.txt");

    int controlled_players[4] = {1, 1, 1, 1}; // Default: control all
    if (argc > 1) {
        controlled_players[0] = 0;
        controlled_players[1] = 0;
        controlled_players[2] = 0;
        controlled_players[3] = 0;
        for (int i = 1; i < argc; i++) {
            int p = atoi(argv[i]);
            if (p >= 0 && p < 4) controlled_players[p] = 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stun;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    signal(SIGTERM, handle_sigterm);

    shm_open_existing();
    wait_for_playing();
    if (!g_running) { shmdt(g_state); return 0; }

    hip_state_enter(g_cfg.proc_hip);
    g_pcount = g_state->player_count;

    if (argc > 1) {
        int my_team = (controlled_players[0] || controlled_players[1]) ? 1 : 2;
        for (int i = 0; i < 4; i++) {
            if (controlled_players[i]) g_state->team[i] = my_team;
        }
        printf("[HIP] Registered as Team %d\n", my_team);
    }

    hip_state_exit(g_cfg.proc_hip);

    // Init local input queue
    inq_init(&g_input);

    printf("[HIP] Spawning player threads for controlled characters...\n");
    for (int i = 0; i < g_pcount; i++) {
        if (controlled_players[i]) {
            pthread_create(&g_threads[i], NULL, player_thread, (void *)(long)i);
        }
    }

    int normal_exit = 0;
    while (g_running)
    {
        int ph;
        int popped = 0;
        int key = -1;

        hip_state_enter(g_cfg.proc_hip);
        ph = g_state->phase;
        
        int act_idx = g_state->active_index;
        int act_is_pl = g_state->active_is_player;
        int go = g_state->hip_go;
        int drop_active = g_state->drop.active;
        int drop_for = g_state->drop.for_player;

        // Determine if this process should be capturing input right now
        int my_turn = 0;
        if (ph == g_cfg.phase_playing) {
            if (drop_active && drop_for >= 0 && drop_for < 4 && controlled_players[drop_for]) {
                my_turn = 1;
            } else if (go && act_is_pl && act_idx >= 0 && act_idx < 4 && controlled_players[act_idx]) {
                my_turn = 1;
            }
        }

        if (my_turn) {
            popped = kbd_pop(&g_state->kbd, &key);
        }
        hip_state_exit(g_cfg.proc_hip);

        if (ph == g_cfg.phase_gameover || ph == g_cfg.phase_win) {
            normal_exit = 1;
            break;
        }

        if (popped) hip_input_push(key);

        struct timespec ts = {0, 10000000};   // 10ms poll
        nanosleep(&ts, NULL);
    }

    g_running = 0;

    if (!normal_exit) {
        hip_state_enter(g_cfg.proc_hip);
        int apid = g_state->arbiter_pid;
        hip_state_exit(g_cfg.proc_hip);
        if (apid > 0) kill(apid, SIGTERM);
    }

    for (int i = 0; i < g_pcount; i++)
        pthread_join(g_threads[i], NULL);

    shmdt(g_state);
    printf("[HIP] Clean shutdown.\n");
    return 0;
}
