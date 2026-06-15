#pragma once

#include <sys/shm.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int shm_key;
    int shm_version;

    // Window (optional). If 0, arbiter picks a default.
    int window_w;
    int window_h;

    int max_players;
    int max_enemies;
    int min_enemies;
    int name_len;
    int win_kill_count;

    int roll_seed;

    // Peterson process IDs
    int proc_arbiter;
    int proc_hip;
    int proc_asp;

    // Game phases
    int phase_start;
    int phase_select;
    int phase_playing;
    int phase_gameover;
    int phase_win;

    // Player actions
    int action_none;
    int action_strike;
    int action_exhaust;
    int action_heal;
    int action_skip;
    int action_quit;

    int action_use_weapon;
    int action_swap_in;

    int action_ultimate;

    // Enemy actions
    int enemy_none;
    int enemy_strike;
    int enemy_skip;

    // Stamina
    int player_max_stamina;
    int enemy_max_stamina;

    // Scheduler
    int tick_ms;
    int npc_timeout_ticks;

    // Player input timeout (0 = disabled)
    int hip_timeout_ticks;

    int weapon_drop_pct;
} Config;

// Reads config.txt and fills in a Config struct.
// Exits the program if the file cannot be opened.
static void load_config(Config *cfg, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open config file: %s\n", path);
        exit(1);
    }

    memset(cfg, 0, sizeof(Config));

    char key[64];
    int  val;
    while (fscanf(f, " %63s", key) == 1) {
        if (key[0] == '#') {
            // Skip comment line
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF);
            continue;
        }
        if (fscanf(f, " %d", &val) != 1) continue;

        if      (strcmp(key, "shm_key")            == 0) cfg->shm_key            = val;
        else if (strcmp(key, "shm_version")        == 0) cfg->shm_version        = val;
        else if (strcmp(key, "window_w")           == 0) cfg->window_w           = val;
        else if (strcmp(key, "window_h")           == 0) cfg->window_h           = val;
        else if (strcmp(key, "max_players")        == 0) cfg->max_players        = val;
        else if (strcmp(key, "max_enemies")        == 0) cfg->max_enemies        = val;
        else if (strcmp(key, "min_enemies")        == 0) cfg->min_enemies        = val;
        else if (strcmp(key, "name_len")           == 0) cfg->name_len           = val;
        else if (strcmp(key, "win_kill_count")     == 0) cfg->win_kill_count     = val;
        else if (strcmp(key, "roll_seed")          == 0) cfg->roll_seed          = val;
        else if (strcmp(key, "proc_arbiter")       == 0) cfg->proc_arbiter       = val;
        else if (strcmp(key, "proc_hip")           == 0) cfg->proc_hip           = val;
        else if (strcmp(key, "proc_asp")           == 0) cfg->proc_asp           = val;
        else if (strcmp(key, "phase_start")        == 0) cfg->phase_start        = val;
        else if (strcmp(key, "phase_select")       == 0) cfg->phase_select       = val;
        else if (strcmp(key, "phase_playing")      == 0) cfg->phase_playing      = val;
        else if (strcmp(key, "phase_gameover")     == 0) cfg->phase_gameover     = val;
        else if (strcmp(key, "phase_win")          == 0) cfg->phase_win          = val;
        else if (strcmp(key, "action_none")        == 0) cfg->action_none        = val;
        else if (strcmp(key, "action_strike")      == 0) cfg->action_strike      = val;
        else if (strcmp(key, "action_exhaust")     == 0) cfg->action_exhaust     = val;
        else if (strcmp(key, "action_heal")        == 0) cfg->action_heal        = val;
        else if (strcmp(key, "action_skip")        == 0) cfg->action_skip        = val;
        else if (strcmp(key, "action_quit")        == 0) cfg->action_quit        = val;
        else if (strcmp(key, "action_use_weapon")  == 0) cfg->action_use_weapon  = val;
        else if (strcmp(key, "action_swap_in")     == 0) cfg->action_swap_in     = val;
        else if (strcmp(key, "action_ultimate")    == 0) cfg->action_ultimate    = val;
        else if (strcmp(key, "enemy_none")         == 0) cfg->enemy_none         = val;
        else if (strcmp(key, "enemy_strike")       == 0) cfg->enemy_strike       = val;
        else if (strcmp(key, "enemy_skip")         == 0) cfg->enemy_skip         = val;
        else if (strcmp(key, "player_max_stamina") == 0) cfg->player_max_stamina = val;
        else if (strcmp(key, "enemy_max_stamina")  == 0) cfg->enemy_max_stamina  = val;
        else if (strcmp(key, "tick_ms")            == 0) cfg->tick_ms            = val;
        else if (strcmp(key, "npc_timeout_ticks")  == 0) cfg->npc_timeout_ticks  = val;
        else if (strcmp(key, "hip_timeout_ticks")  == 0) cfg->hip_timeout_ticks  = val;
        else if (strcmp(key, "weapon_drop_pct")    == 0) cfg->weapon_drop_pct    = val;
    }

    fclose(f);
}

typedef struct {
    int interested[3];
    int turn;
} PetersonLock;

static inline void peterson_enter(PetersonLock *lk, int me, int other)
{
    lk->interested[me] = 1;
    lk->turn = other;
    while (lk->interested[other] == 1 && lk->turn == other)
        ;
}

static inline void peterson_exit(PetersonLock *lk, int me)
{
    lk->interested[me] = 0;
}

typedef struct {
    int choosing[3];
    int number[3];
} BakeryLock3;

static inline void bakery3_init(BakeryLock3 *lk)
{
    for (int i = 0; i < 3; i++) {
        lk->choosing[i] = 0;
        lk->number[i] = 0;
    }
}

static inline void bakery3_compiler_barrier()
{
    __asm__ __volatile__("" ::: "memory");
}

static inline void bakery3_cpu_pause()
{
    bakery3_compiler_barrier();
#if defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    // Fallback: compiler barrier only
    __asm__ __volatile__("" ::: "memory");
#endif
}

static inline int bakery3_max_number(BakeryLock3 *lk)
{
    int m = 0;
    for (int i = 0; i < 3; i++) {
        int v = lk->number[i];
        if (v > m) m = v;
    }
    return m;
}

static inline int bakery3_before(int num_a, int a, int num_b, int b)
{
    // Return 1 iff (num_a, a) < (num_b, b)
    if (num_a < num_b) return 1;
    if (num_a > num_b) return 0;
    return a < b;
}

static inline void bakery3_enter(BakeryLock3 *lk, int me)
{
    lk->choosing[me] = 1;
    bakery3_compiler_barrier();

    int my = bakery3_max_number(lk) + 1;
    lk->number[me] = my;
    bakery3_compiler_barrier();

    lk->choosing[me] = 0;
    bakery3_compiler_barrier();

    for (int j = 0; j < 3; j++) {
        if (j == me) continue;

        // Wait until j finished choosing its ticket
        while (1) {
            int cj = lk->choosing[j];
            bakery3_cpu_pause();
            if (!cj) break;
        }

        // Wait while j has a ticket and is before me
        while (1) {
            int nj = lk->number[j];
            bakery3_cpu_pause();
            if (nj == 0) break;
            if (!bakery3_before(nj, j, my, me)) break;
        }
    }
}

static inline void bakery3_exit(BakeryLock3 *lk, int me)
{
    lk->number[me] = 0;
    bakery3_compiler_barrier();
}

typedef struct {
    char name[32];
    int  hp;
    int  max_hp;
    int  damage;
    int  speed;
    int  stamina;
    int  max_stamina;
    int  alive;
    int  stunned;
    double stun_end;

    // Milestone 6
    int inventory[20];
    int storage[32];
    int storage_count;
    int held_weapon; // enemies: if non-zero, weapon will not drop on death
    int just_swapped_in;
} Entity;

typedef struct {
    int keys[32];
    int head;      // pop index
    int tail;      // push index
} KbdBuffer;

static inline void kbd_init(KbdBuffer *kb)
{
    kb->head = 0;
    kb->tail = 0;
    for (int i = 0; i < 32; i++) kb->keys[i] = -1;
}

// Returns 1 if pushed, 0 if queue full (key dropped)
static inline int kbd_push(KbdBuffer *kb, int key)
{
    int next = (kb->tail + 1) % 32;
    if (next == kb->head) return 0; // full
    kb->keys[kb->tail] = key;
    kb->tail = next;
    return 1;
}

// Returns 1 if popped, 0 if empty
static inline int kbd_pop(KbdBuffer *kb, int *out_key)
{
    if (kb->head == kb->tail) return 0; // empty
    *out_key = kb->keys[kb->head];
    kb->keys[kb->head] = -1;
    kb->head = (kb->head + 1) % 32;
    return 1;
}

typedef struct {
    int action;        // one of cfg.action_*
    int target_index;
    int aux;
    int ready;
} HIPBuffer;

typedef struct {
    int action;        // one of cfg.enemy_*
    int attacker_index;
    int target_index;
    int ready;
} ASPBuffer;

#define LOG_COUNT 8
#define LOG_LEN   120

typedef struct {
    char lines[LOG_COUNT][LOG_LEN];
    int  head;   // next slot to write into (circular)
} ActionLog;

typedef struct {
    int active;       
    int weapon_id;   
    int for_player;   
} DropState;

typedef struct {
    int solar_owner;
    int lunar_owner;
    int eclipse_owner;

    int solar_waiter;
    int lunar_waiter;
    int eclipse_waiter;

    int eclipse_present;
} ResourceTable;

typedef struct {
    int version;
    int phase;         

    Entity players[4];
    int    team[4];
    int    player_count;

    Entity enemies[9];
    int    enemy_count;
    int    enemies_killed;
    int    team_kills[3];

    int    active_is_player;
    int    active_index;

    int    scheduler_tick;

    HIPBuffer hip_buf;
    ASPBuffer asp_buf;

    BakeryLock3 lock;

    // Resource table has its own lock (separate from main state lock)
    BakeryLock3 res_lock;
    ResourceTable res;

    int hip_go;
    int asp_go;

    int arbiter_pid;
    int hip_pid;
    int asp_pid;

    KbdBuffer kbd;    // Arbiter writes, HIP reads — no separate window needed

    ActionLog log;

    DropState drop;

} GameState;
