#include "shared_state.h"

#include <SFML/Graphics.hpp>
#include <X11/Xlib.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <thread>
#include <time.h>
#include <unistd.h>

static Config g_cfg;
static GameState *g_state = NULL;
static int g_shmid = -1;
static int g_running = 1;

static int g_asp_alarm_fired = 0;
static long long g_hip_stun_until_ms = 0;
static long long g_asp_stun_until_ms = 0;
static int g_asp_suspended = 0;

// Arbiter has 2 threads (scheduler + render). Prevent same-process races.
static PetersonLock g_thread_lock;
static const int THREAD_SCHED = 0;
static const int THREAD_REND  = 1;

// UI key queue for Arbiter-only menus (start/select). Single-slot.
static int g_ui_has_key = 0;
static int g_ui_key = -1;

// Scheduler bookkeeping (Arbiter-local): when an entity first reached full stamina.
// -1 means "not currently ready".
static int g_ready_tick_players[4] = {-1, -1, -1, -1};
static int g_ready_tick_enemies[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
// Enemy lifecycle (Arbiter-local)
static int g_enemy_respawn_secs[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
static int g_enemy_serial = 1;

static int g_pending_ult_player[4] = {0, 0, 0, 0};
static int g_pending_ult_enemy[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

// Tracking last used weapon for UI grid highlight
static int g_ui_last_weapon[4] = {0, 0, 0, 0};

// Pause state (toggled by ESC in render thread, read by scheduler)
static int g_paused = 0;

// Scheduler start time for elapsed-time tracking on end screens
static long long g_game_start_ms = 0;

static long long now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static void handle_sigterm(int sig)
{
    (void)sig;
    g_running = 0;
}

static void handle_sigalrm(int sig)
{
    (void)sig;
    g_asp_alarm_fired = 1;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int rand_range(int lo, int hi)
{
    return lo + rand() % (hi - lo + 1);
}

static int roll_last_digit() { return g_cfg.roll_seed % 10; }
static int roll_second_last_digit() { return (g_cfg.roll_seed / 10) % 10; }
static int roll_last_two_digits() { return g_cfg.roll_seed % 100; }
static int roll_base_for_hp()
{
    int v = g_cfg.roll_seed;
    if (v < 0) v = -v;
    if (v > 9999) v = v % 10000;
    if (v <= 0) v = 1;
    return v;
}

typedef struct {
    const char *name;
    int slots;
    int damage;
} WeaponDef;

static const WeaponDef g_weapons[9] = {
    {"None",          0,  0},   // 0
    {"Solar Core",   10, 95},   // 1
    {"Lunar Blade",  10, 90},   // 2
    {"Iron Halberd",  7, 55},   // 3
    {"Venom Dagger",  4, 30},   // 4
    {"Thunderstaff",  6, 50},   // 5
    {"Obsidian Axe",  5, 45},   // 6
    {"Frostbow",      6, 48},   // 7
    {"Splinter Stick", 2, 12},  // 8
};

static int weapon_slots(int weapon_id)
{
    if (weapon_id < 1 || weapon_id > 8) return 0;
    return g_weapons[weapon_id].slots;
}

static int weapon_damage(int weapon_id)
{
    if (weapon_id < 1 || weapon_id > 8) return 0;
    return g_weapons[weapon_id].damage;
}

static const char *weapon_name(int weapon_id)
{
    if (weapon_id < 1 || weapon_id > 8) return "Unknown";
    return g_weapons[weapon_id].name;
}


static void storage_push(Entity *e, int weapon_id)
{
    if (e->storage_count < 0) e->storage_count = 0;
    if (e->storage_count >= 32) return;
    e->storage[e->storage_count++] = weapon_id;
}

static int storage_pop_at(Entity *e, int idx)
{
    if (idx < 0 || idx >= e->storage_count || idx >= 32) return 0;
    int id = e->storage[idx];
    for (int i = idx; i + 1 < e->storage_count && i + 1 < 32; i++)
        e->storage[i] = e->storage[i + 1];
    e->storage[e->storage_count - 1] = 0;
    e->storage_count--;
    if (e->storage_count < 0) e->storage_count = 0;
    return id;
}

static int storage_find_weapon(const Entity *e, int weapon_id)
{
    for (int i = 0; i < e->storage_count && i < 32; i++) {
        if (e->storage[i] == weapon_id) return i;
    }
    return -1;
}

static int storage_best_weapon_index(const Entity *e)
{
    int best_i = -1, best_d = 0;
    for (int i = 0; i < e->storage_count && i < 32; i++) {
        int d = weapon_damage(e->storage[i]);
        if (d > best_d) { best_d = d; best_i = i; }
    }
    return best_i;
}

// Find first contiguous block of >= `need` free (0) slots.
// Returns starting index, or -1 if not found.
static int inventory_find_first_fit(const int inv[20], int need)
{
    if (need <= 0 || need > 20) return -1;
    int run = 0;
    for (int i = 0; i < 20; i++) {
        if (inv[i] == 0) {
            run++;
            if (run >= need) return i - need + 1;
        } else {
            run = 0;
        }
    }
    return -1;
}

// Check whether a specific weapon_id exists anywhere in inventory.
static int inv_has_weapon_id(const int inv[20], int weapon_id)
{
    for (int i = 0; i < 20; i++) {
        if (inv[i] == weapon_id) return 1;
    }
    return 0;
}

// Return the best (highest damage) weapon_id currently in inventory.
static int inventory_best_weapon_id(const int inv[20])
{
    int best_id = 0, best_dmg = 0;
    for (int i = 0; i < 20; i++) {
        int id = inv[i];
        if (id == 0) continue;
        // Only count at the start of a run to avoid duplicates
        if (i > 0 && inv[i - 1] == id) continue;
        int dmg = weapon_damage(id);
        if (dmg > best_dmg) { best_dmg = dmg; best_id = id; }
    }
    return best_id;
}

// Identify all distinct weapon "runs" in the inventory.
// A run is a contiguous sequence of the same non-zero weapon_id.
// Fills out_id[], out_start[], out_len[] and returns the count.
static int inventory_list_runs(const int inv[20], int out_id[20], int out_start[20], int out_len[20])
{
    int count = 0;
    int i = 0;
    while (i < 20) {
        if (inv[i] == 0) { i++; continue; }
        int id = inv[i];
        int start = i;
        while (i < 20 && inv[i] == id) i++;
        out_id[count]    = id;
        out_start[count] = start;
        out_len[count]   = i - start;
        count++;
    }
    return count;
}

// Remove a specific weapon run from inventory (set slots to 0) and push to storage.
static void inventory_evict_run(Entity *e, int run_start, int run_len, int weapon_id)
{
    for (int j = run_start; j < run_start + run_len && j < 20; j++)
        e->inventory[j] = 0;
    storage_push(e, weapon_id);
}

// Place a weapon into the player's inventory using first-fit contiguous allocation.
// If no contiguous space exists, swap out the MINIMUM number of weapons needed.
// Swapped-out weapons go to long-term storage.
// Returns 1 on success, 0 on failure (e.g., storage overflow).
static int inventory_place_weapon(Entity *e, int weapon_id)
{
    int need = weapon_slots(weapon_id);
    if (need <= 0 || need > 20) return 0;

    // Step 1: Try direct first-fit placement.
    int pos = inventory_find_first_fit(e->inventory, need);
    if (pos >= 0) {
        for (int i = 0; i < need; i++) e->inventory[pos + i] = weapon_id;
        return 1;
    }

    int run_id[20], run_start[20], run_len[20];
    int run_count = inventory_list_runs(e->inventory, run_id, run_start, run_len);

    int best_win    = -1;
    int best_evict  = 999;
    int best_freed  = 999;

    for (int w = 0; w + need <= 20; w++) {
        int w_end = w + need - 1;
        int evict_count = 0;
        int freed_slots = 0;

        for (int r = 0; r < run_count; r++) {
            int rs = run_start[r];
            int re = rs + run_len[r] - 1;
            // Check overlap: run [rs..re] vs window [w..w_end]
            if (re >= w && rs <= w_end) {
                evict_count++;
                freed_slots += run_len[r];
            }
        }

        if (evict_count < best_evict ||
            (evict_count == best_evict && freed_slots < best_freed) ||
            (evict_count == best_evict && freed_slots == best_freed && (best_win < 0 || w < best_win))) {
            best_evict = evict_count;
            best_freed = freed_slots;
            best_win   = w;
        }
    }

    if (best_win < 0) return 0;
    // Safety: make sure storage has room for the evicted weapons
    if (e->storage_count + best_evict > 32) return 0;

    int w_end = best_win + need - 1;
    for (int r = 0; r < run_count; r++) {
        int rs = run_start[r];
        int re = rs + run_len[r] - 1;
        if (re >= best_win && rs <= w_end) {
            // Evict entire run (not just the overlapping portion)
            inventory_evict_run(e, rs, run_len[r], run_id[r]);
        }
    }

    pos = inventory_find_first_fit(e->inventory, need);
    if (pos < 0) return 0; // should not happen after eviction
    for (int i = 0; i < need; i++) e->inventory[pos + i] = weapon_id;
    return 1;
}

static int roll_weapon_drop_id()
{
    return rand_range(1, 8);
}

static int maybe_drop_weapon()
{
    int pct = g_cfg.weapon_drop_pct;
    if (pct <= 0) return 0;
    if (pct > 100) pct = 100;
    return (rand_range(1, 100) <= pct);
}

static void thread_enter(int me)
{
    peterson_enter(&g_thread_lock, me, (me == THREAD_SCHED) ? THREAD_REND : THREAD_SCHED);
}

static void thread_exit(int me)
{
    peterson_exit(&g_thread_lock, me);
}

static void state_enter(int me)
{
    thread_enter(me);
    bakery3_enter(&g_state->lock, g_cfg.proc_arbiter);
}

static void state_exit(int me)
{
    bakery3_exit(&g_state->lock, g_cfg.proc_arbiter);
    thread_exit(me);
}

static void log_actionf(int me, const char *fmt, ...)
{
    char buf[LOG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    state_enter(me);
    int h = g_state->log.head;
    strncpy(g_state->log.lines[h], buf, LOG_LEN - 1);
    g_state->log.lines[h][LOG_LEN - 1] = '\0';
    g_state->log.head = (h + 1) % LOG_COUNT;
    state_exit(me);
}

static void shm_create()
{
    // Remove any stale segment
    int old = shmget(g_cfg.shm_key, 0, 0666);
    if (old >= 0) shmctl(old, IPC_RMID, NULL);

    g_shmid = shmget(g_cfg.shm_key, sizeof(GameState), 0666 | IPC_CREAT);
    if (g_shmid < 0) {
        perror("arbiter: shmget");
        exit(1);
    }

    g_state = (GameState *)shmat(g_shmid, NULL, 0);
    if (g_state == (GameState *)-1) {
        perror("arbiter: shmat");
        exit(1);
    }

    memset(g_state, 0, sizeof(GameState));
    g_state->version = g_cfg.shm_version;
    g_state->phase = g_cfg.phase_start;
    g_state->arbiter_pid = getpid();

    kbd_init(&g_state->kbd);

    bakery3_init(&g_state->lock);
    bakery3_init(&g_state->res_lock);
    memset(&g_state->res, 0, sizeof(g_state->res));
    g_state->hip_pid = 0;
    g_state->asp_pid = 0;

    printf("[Arbiter] Shared memory created. shmid=%d size=%lu bytes\n",
           g_shmid, sizeof(GameState));
}

static void shm_destroy()
{
    shmdt(g_state);
    shmctl(g_shmid, IPC_RMID, NULL);
    printf("[Arbiter] Shared memory removed.\n");
}

static void init_game_state(int me, int player_count)
{
    srand(g_cfg.roll_seed);

    state_enter(me);

    g_state->phase = g_cfg.phase_playing;
    g_state->player_count = player_count;
    g_state->enemy_count = rand_range(g_cfg.min_enemies, g_cfg.max_enemies);
    g_state->enemies_killed = 0;

    for (int i = 0; i < 4; i++) g_ready_tick_players[i] = -1;
    for (int i = 0; i < 9; i++) g_ready_tick_enemies[i] = -1;
    for (int i = 0; i < 9; i++) g_enemy_respawn_secs[i] = 0;
    g_enemy_serial = 1;

    for (int i = 0; i < 4; i++) g_pending_ult_player[i] = 0;
    for (int i = 0; i < 9; i++) g_pending_ult_enemy[i] = 0;

    g_hip_stun_until_ms = 0;
    g_asp_stun_until_ms = 0;
    g_asp_suspended = 0;

    for (int i = 0; i < g_cfg.max_players; i++) {
        Entity *p = &g_state->players[i];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "Hero %d", i + 1);

        if (i < player_count) {
            p->max_hp = roll_base_for_hp() + rand_range(100, 1000);
            p->hp = p->max_hp;
            p->damage = roll_last_digit() + 10;
            p->speed = 100 / player_count;
            p->max_stamina = g_cfg.player_max_stamina;
            p->stamina = 0;
            p->alive = 1;
            p->storage_count = 0;
            p->held_weapon = 0;
            for (int s = 0; s < 20; s++) p->inventory[s] = 0;
            for (int s = 0; s < 32; s++) p->storage[s] = 0;
        } else {
            p->alive = 0;
        }
    }

    for (int i = 0; i < g_cfg.max_enemies; i++) {
        Entity *e = &g_state->enemies[i];
        memset(e, 0, sizeof(*e));

        if (i < g_state->enemy_count) {
            snprintf(e->name, sizeof(e->name), "Enemy %d", g_enemy_serial++);
            e->max_hp = roll_last_two_digits() + rand_range(50, 200);
            e->hp = e->max_hp;
            e->damage = roll_second_last_digit() + 10;
            e->speed = rand_range(10, 30);
            e->max_stamina = g_cfg.enemy_max_stamina;
            e->stamina = 0;
            e->alive = 1;
            e->held_weapon = 0;
        } else {
            snprintf(e->name, sizeof(e->name), "Enemy %d", i + 1);
            e->alive = 0;
        }
    }

    g_state->active_is_player = 1;
    g_state->active_index = 0;

    g_state->scheduler_tick = 0;

    g_state->hip_buf.action = g_cfg.action_none;
    g_state->hip_buf.target_index = 0;
    g_state->hip_buf.aux = 0;
    g_state->hip_buf.ready = 0;

    g_state->asp_buf.action = g_cfg.enemy_none;
    g_state->asp_buf.attacker_index = 0;
    g_state->asp_buf.target_index = 0;
    g_state->asp_buf.ready = 0;

    g_state->hip_go = 0;
    g_state->asp_go = 0;

    g_state->drop.active = 0;
    g_state->drop.weapon_id = 0;
    g_state->drop.for_player = 0;

    // Reset artifact resource table
    bakery3_enter(&g_state->res_lock, g_cfg.proc_arbiter);
    memset(&g_state->res, 0, sizeof(g_state->res));
    bakery3_exit(&g_state->res_lock, g_cfg.proc_arbiter);

    g_state->log.head = 0;
    memset(g_state->log.lines, 0, sizeof(g_state->log.lines));

    int enemy_count = g_state->enemy_count;

    state_exit(me);

    log_actionf(me, "Party size: %d | Enemies: %d", player_count, enemy_count);
}

static int owner_for_player(int pidx) { return pidx + 1; }
static int owner_for_enemy(int eidx) { return -(eidx + 1); }

static int inv_has_weapon(const Entity *p, int weapon_id)
{
    return inv_has_weapon_id(p->inventory, weapon_id);
}

static void res_set_waiter_unsafe(int weapon_id, int owner)
{
    if (weapon_id == 1) g_state->res.solar_waiter = owner;
    else if (weapon_id == 2) g_state->res.lunar_waiter = owner;
    else g_state->res.eclipse_waiter = owner;
}

static int res_get_owner_unsafe(int weapon_id)
{
    if (weapon_id == 1) return g_state->res.solar_owner;
    if (weapon_id == 2) return g_state->res.lunar_owner;
    return g_state->res.eclipse_owner;
}

static void res_set_owner_unsafe(int weapon_id, int owner)
{
    if (weapon_id == 1) g_state->res.solar_owner = owner;
    else if (weapon_id == 2) g_state->res.lunar_owner = owner;
    else g_state->res.eclipse_owner = owner;
}

static int res_try_lock_weapon(int me, int weapon_id, int owner)
{
    (void)me;
    // weapon_id: 1=Solar, 2=Lunar (others ignored for locking)
    if (weapon_id != 1 && weapon_id != 2) return 1;

    // Separate lock for resource table
    bakery3_enter(&g_state->res_lock, g_cfg.proc_arbiter);
    int cur = res_get_owner_unsafe(weapon_id);
    if (cur == 0 || cur == owner) {
        res_set_owner_unsafe(weapon_id, owner);
        // If I was the waiter and now acquired, clear waiter
        if (weapon_id == 1 && g_state->res.solar_waiter == owner) g_state->res.solar_waiter = 0;
        if (weapon_id == 2 && g_state->res.lunar_waiter == owner) g_state->res.lunar_waiter = 0;
        bakery3_exit(&g_state->res_lock, g_cfg.proc_arbiter);
        return 1;
    }

    // Locked by someone else: record waiter
    res_set_waiter_unsafe(weapon_id, owner);
    bakery3_exit(&g_state->res_lock, g_cfg.proc_arbiter);
    return 0;
}

static void res_unlock_weapon(int me, int weapon_id, int owner)
{
    (void)me;
    if (weapon_id != 1 && weapon_id != 2) return;
    bakery3_enter(&g_state->res_lock, g_cfg.proc_arbiter);
    int cur = res_get_owner_unsafe(weapon_id);
    if (cur == owner) {
        res_set_owner_unsafe(weapon_id, 0);
    }
    bakery3_exit(&g_state->res_lock, g_cfg.proc_arbiter);
}

static void deadlock_check_and_resolve_unsafe(int me)
{
    (void)me;
    // Called with state lock held.
    bakery3_enter(&g_state->res_lock, g_cfg.proc_arbiter);

    int so = g_state->res.solar_owner;
    int lo = g_state->res.lunar_owner;
    int sw = g_state->res.solar_waiter;
    int lw = g_state->res.lunar_waiter;

    int deadlocked = (so != 0 && lo != 0 && sw == lo && lw == so);
    if (deadlocked) {
        // Resolve by forcing Lunar Blade to be released.
        g_state->res.lunar_owner = 0;
        g_state->res.lunar_waiter = 0;

        snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                 "Deadlock detected: forced Lunar Blade release");
        g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
    }

    bakery3_exit(&g_state->res_lock, g_cfg.proc_arbiter);
}

static void send_stun_to_pid(int pid)
{
    if (pid <= 0) return;
    kill(pid, SIGUSR1);
}

static void resume_asp_if_alarm(int me)
{
    if (!g_asp_alarm_fired) return;
    g_asp_alarm_fired = 0;

    state_enter(me);
    int asp_pid = g_state->asp_pid;
    if (g_asp_suspended && asp_pid > 0) {
        // Spec: update enemy staminas before resuming ASP.
        for (int i = 0; i < g_state->enemy_count; i++) {
            Entity *e = &g_state->enemies[i];
            if (!e->alive) continue;
            e->stamina += e->speed * 10; // catch-up for 10s suspension
            if (e->stamina > e->max_stamina) e->stamina = e->max_stamina;
            if (e->stamina >= e->max_stamina && g_ready_tick_enemies[i] < 0)
                g_ready_tick_enemies[i] = g_state->scheduler_tick;
        }

        g_state->asp_go = 0;
        g_state->asp_buf.ready = 0;
        g_asp_suspended = 0;
        kill(asp_pid, SIGCONT);

        snprintf(g_state->log.lines[g_state->log.head], LOG_LEN, "ASP resumed");
        g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
    }
    state_exit(me);
}

static void try_complete_pending_ultimates_unsafe(int me)
{
    // Players
    for (int pidx = 0; pidx < g_state->player_count && pidx < 4; pidx++) {
        if (!g_pending_ult_player[pidx]) continue;
        Entity *p = &g_state->players[pidx];
        if (!p->alive) { g_pending_ult_player[pidx] = 0; continue; }

        // Still must have both artifacts in active inventory.
        if (!inv_has_weapon(p, 1) || !inv_has_weapon(p, 2)) {
            g_pending_ult_player[pidx] = 0;
            continue;
        }

        int owner = owner_for_player(pidx);
        int ok1 = res_try_lock_weapon(me, 1, owner);
        int ok2 = res_try_lock_weapon(me, 2, owner);
        if (!(ok1 && ok2)) continue;

        int asp_pid = g_state->asp_pid;
        if (asp_pid > 0) {
            kill(asp_pid, SIGSTOP);
            g_asp_suspended = 1;
            alarm(10);
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                     "%s completes ULTIMATE: ASP suspended 10s", p->name);
            g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
        }

        res_unlock_weapon(me, 1, owner);
        res_unlock_weapon(me, 2, owner);
        g_pending_ult_player[pidx] = 0;
    }

    // Enemies (demonstrates Milestone 8 deadlock with NPCs)
    for (int eidx = 0; eidx < g_state->enemy_count && eidx < 9; eidx++) {
        if (!g_pending_ult_enemy[eidx]) continue;
        Entity *e = &g_state->enemies[eidx];
        if (!e->alive) { g_pending_ult_enemy[eidx] = 0; continue; }

        int held = e->held_weapon;
        if (held != 1 && held != 2) { g_pending_ult_enemy[eidx] = 0; continue; }
        int other = (held == 1) ? 2 : 1;

        int owner = owner_for_enemy(eidx);
        int ok1 = res_try_lock_weapon(me, held, owner);
        int ok2 = res_try_lock_weapon(me, other, owner);
        if (!(ok1 && ok2)) continue;

        // Enemy "ultimate" effect: stun HIP for 3 seconds.
        int hip_pid = g_state->hip_pid;
        g_hip_stun_until_ms = now_ms() + 3000;
        send_stun_to_pid(hip_pid);
        snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                 "%s triggers artifact clash: HIP stunned (3s)", e->name);
        g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;

        res_unlock_weapon(me, held, owner);
        res_unlock_weapon(me, other, owner);
        g_pending_ult_enemy[eidx] = 0;
    }
}

static int all_players_dead_unsafe()
{
    for (int i = 0; i < g_state->player_count; i++) {
        if (g_state->players[i].alive) return 0;
    }
    return 1;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void respawn_enemy_slot_unsafe(int slot)
{
    Entity *e = &g_state->enemies[slot];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "Enemy %d", g_enemy_serial++);
    e->max_hp = roll_last_two_digits() + rand_range(50, 200);
    e->hp = e->max_hp;
    e->damage = roll_second_last_digit() + 10;
    e->speed = rand_range(10, 30);
    e->max_stamina = g_cfg.enemy_max_stamina;
    e->stamina = 0;
    e->alive = 1;
    e->held_weapon = 0;

    if (slot >= 0 && slot < 9) g_ready_tick_enemies[slot] = -1;
}

static int enemy_pickup_weapon_unsafe(int weapon_id)
{
    // Prefer an alive enemy with empty hands; otherwise any alive enemy.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < g_state->enemy_count; i++) {
            if (!g_state->enemies[i].alive) continue;
            if (pass == 0 && g_state->enemies[i].held_weapon != 0) continue;
            g_state->enemies[i].held_weapon = weapon_id;
            return i;
        }
    }
    return -1;
}

static void begin_drop_prompt_unsafe(int player_idx, int weapon_id)
{
    g_state->drop.active = 1;
    g_state->drop.weapon_id = weapon_id;
    g_state->drop.for_player = player_idx;
    g_state->hip_buf.ready = 0;
    g_state->hip_go = 1;

    snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
             "%s dropped! %s: Pick (P) or Leave (L)",
             weapon_name(weapon_id), g_state->players[player_idx].name);
    g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
}

static void apply_drop_decision(int me)
{
    state_enter(me);

    if (!g_state->drop.active) {
        state_exit(me);
        return;
    }

    int pidx = g_state->drop.for_player;
    int wid = g_state->drop.weapon_id;
    int pick = (g_state->hip_buf.aux != 0);

    g_state->hip_buf.ready = 0;
    g_state->hip_go = 0;
    g_state->drop.active = 0;
    g_state->drop.weapon_id = 0;
    g_state->drop.for_player = 0;

    if (pidx < 0 || pidx >= g_state->player_count) {
        state_exit(me);
        return;
    }

    Entity *p = &g_state->players[pidx];

    if (!pick) {
        // Player declined: enemy is guaranteed to pick it up (Spec Section 6)
        int ei = enemy_pickup_weapon_unsafe(wid);
        if (ei >= 0)
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                     "%s left it. %s picked it up.", p->name, g_state->enemies[ei].name);
        else
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                     "%s left it. No enemy could pick it up.", p->name);
        g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
        state_exit(me);
        return;
    }

    // Player wants to pick it up
    int ok = inventory_place_weapon(p, wid);
    if (ok) {
        snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                 "%s picked up %s", p->name, weapon_name(wid));

        if (wid == 1 || wid == 2) {
            // Solar Core or Lunar Blade picked up — resource table tracks ownership
            // (Actual locking happens when the weapon is *used*, not when picked up.)
        }
    } else {
        // Can't carry it — enemy guaranteed to pick it up
        int ei = enemy_pickup_weapon_unsafe(wid);
        if (ei >= 0)
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                     "%s couldn't carry it. %s picked it up.", p->name, g_state->enemies[ei].name);
        else
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                     "%s couldn't carry it.", p->name);
    }
    g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;

    state_exit(me);
}

static void apply_player_action(int me)
{
    state_enter(me);

    int pidx = g_state->active_index;
    if (pidx < 0 || pidx >= g_state->player_count || !g_state->players[pidx].alive) {
        g_state->hip_go = 0;
        g_state->hip_buf.ready = 0;
        state_exit(me);
        return;
    }

    Entity *p = &g_state->players[pidx];

    if (pidx >= 0 && pidx < 4) g_ready_tick_players[pidx] = -1;

    int action = g_state->hip_buf.action;
    int target = g_state->hip_buf.target_index;
    int aux = g_state->hip_buf.aux;

    g_state->hip_buf.ready = 0;
    g_state->hip_go = 0;

    // Clear the just_swapped_in flag at the start of every new turn.
    // This means a weapon swapped in last turn is now usable.
    p->just_swapped_in = 0;

    if (action == g_cfg.action_quit) {
        g_running = 0;
        state_exit(me);
        return;
    }

    if (action == g_cfg.action_strike || action == g_cfg.action_exhaust || action == g_cfg.action_use_weapon) {
        if (target < 0) target = 0;
        if (target >= g_state->enemy_count) target = g_state->enemy_count - 1;

        Entity *e = &g_state->enemies[target];
        if (!e->alive) {
            for (int i = 0; i < g_state->enemy_count; i++) {
                if (g_state->enemies[i].alive) {
                    e = &g_state->enemies[i];
                    target = i;
                    break;
                }
            }
        }

        if (e->alive) {
            if (action == g_cfg.action_strike) {
                e->hp -= p->damage;
                e->hp = clamp_int(e->hp, 0, e->max_hp);
                snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                         "%s strikes %s (-%d HP)", p->name, e->name, p->damage);
            }
            else if (action == g_cfg.action_exhaust) {
                e->stamina -= p->damage;
                e->stamina = clamp_int(e->stamina, 0, e->max_stamina);
                snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                         "%s exhausts %s (-%d STA)", p->name, e->name, p->damage);
            }
            else {
                // Use Weapon action
                int wid = 0;
                if (aux >= 1 && aux <= 8) {
                    // Verify weapon is actually in inventory
                    if (inv_has_weapon_id(p->inventory, aux))
                        wid = aux;
                }
                // Fallback: use best weapon in inventory
                if (wid == 0) wid = inventory_best_weapon_id(p->inventory);

                g_ui_last_weapon[pidx] = wid; // Track for rendering

                int wdmg = weapon_damage(wid);
                if (wid == 0 || wdmg <= 0) {
                    snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                             "%s has no weapon", p->name);
                } else {
                    int owner = owner_for_player(pidx);
                    int ok_lock = res_try_lock_weapon(me, wid, owner);
                    if (!ok_lock) {
                        snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                                 "%s blocked: %s locked", p->name, weapon_name(wid));
                        g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
                        p->stamina = p->max_stamina / 2;
                        state_exit(me);
                        return;
                    }

                    e->hp -= wdmg;
                    e->hp = clamp_int(e->hp, 0, e->max_hp);
                    snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                             "%s uses %s on %s (-%d HP)", p->name, weapon_name(wid), e->name, wdmg);

                    if ((wid == 1 || wid == 2) && e->alive) {
                        int asp_pid = g_state->asp_pid;
                        g_asp_stun_until_ms = now_ms() + 3000;
                        e->stunned = 1;
                        e->stun_end = (double)g_asp_stun_until_ms / 1000.0;
                        send_stun_to_pid(asp_pid);
                        snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                                 "%s is stunned (3s)", e->name);
                        g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
                    }

                    res_unlock_weapon(me, wid, owner);
                }
            }
            g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;

            // Check for enemy death
            if (e->alive && e->hp == 0) {
                e->alive = 0;
                if (target >= 0 && target < 9) g_ready_tick_enemies[target] = -1;
                if (target >= 0 && target < 9) g_enemy_respawn_secs[target] = 1;
                g_state->enemies_killed++;

                int killer_team = g_state->team[pidx];
                if (killer_team >= 1 && killer_team <= 2) {
                    g_state->team_kills[killer_team]++;
                }

                snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                         "%s defeated", e->name);
                g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;

                if (g_state->phase == g_cfg.phase_playing && e->held_weapon == 0 && maybe_drop_weapon()) {
                    int drop_wid = roll_weapon_drop_id();

                    bakery3_enter(&g_state->res_lock, g_cfg.proc_arbiter);
                    int eclipse_present = g_state->res.eclipse_present;
                    if (!eclipse_present && rand_range(1, 5) == 1) {
                        g_state->res.eclipse_present = 1;
                        snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                                 "Eclipse Relic has appeared in the world!");
                        g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
                    }
                    bakery3_exit(&g_state->res_lock, g_cfg.proc_arbiter);

                    begin_drop_prompt_unsafe(pidx, drop_wid);
                }

                if (g_state->enemies_killed >= g_cfg.win_kill_count) {
                    g_state->phase = g_cfg.phase_win;
                }
            }
        }

        p->stamina = 0;
    }
    else if (action == g_cfg.action_swap_in) {
        int si = -1;
        if (aux >= 1 && aux <= 8) {
            si = storage_find_weapon(p, aux);
        }
        if (si < 0) si = storage_best_weapon_index(p);
        if (si < 0) {
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                     "%s has nothing in storage", p->name);
            g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
            p->stamina = 0;
            state_exit(me);
            return;
        }

        int wid = storage_pop_at(p, si);
        int ok = inventory_place_weapon(p, wid);
        if (ok) {
            p->just_swapped_in = 1; // cannot use until next turn
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                     "%s swapped in %s (usable next turn)", p->name, weapon_name(wid));
        } else {
            // Failed to place — put it back in storage
            storage_push(p, wid);
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                     "%s couldn't swap in %s", p->name, weapon_name(wid));
        }
        g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
        p->stamina = 0;
    }
    else if (action == g_cfg.action_ultimate) {
        int has_solar = inv_has_weapon(p, 1);
        int has_lunar = inv_has_weapon(p, 2);
        if (!has_solar || !has_lunar) {
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                     "%s cannot Ultimate (need Solar+Lunar)", p->name);
            g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
            p->stamina = 0;
            state_exit(me);
            return;
        }

        int owner = owner_for_player(pidx);
        int ok1 = res_try_lock_weapon(me, 1, owner);
        int ok2 = res_try_lock_weapon(me, 2, owner);
        if (!ok1 || !ok2) {
            g_pending_ult_player[pidx] = 1;
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                     "%s is waiting on artifact locks...", p->name);
            g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
            p->stamina = 0;
            state_exit(me);
            return;
        }

        int asp_pid = g_state->asp_pid;
        if (asp_pid > 0) {
            kill(asp_pid, SIGSTOP);
            g_asp_suspended = 1;
            alarm(10);
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                     "%s uses ULTIMATE: ASP suspended 10s", p->name);
            g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
        }

        res_unlock_weapon(me, 1, owner);
        res_unlock_weapon(me, 2, owner);
        g_pending_ult_player[pidx] = 0;
        p->stamina = 0;
    }
    else if (action == g_cfg.action_heal) {
        int heal = (p->max_hp * 10) / 100;
        if (heal < 1) heal = 1;
        p->hp = clamp_int(p->hp + heal, 0, p->max_hp);
        p->stamina = 0;

        snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                 "%s heals (+%d HP)", p->name, heal);
        g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
    }
    else {
        // Skip or unknown action
        p->stamina = p->max_stamina / 2;
        snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                 "%s skips (50%% stamina)", p->name);
        g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
    }

    state_exit(me);
}

static void apply_enemy_action(int me, int timed_out)
{
    state_enter(me);

    int eidx = g_state->active_index;
    if (eidx < 0 || eidx >= g_state->enemy_count || !g_state->enemies[eidx].alive) {
        g_state->asp_go = 0;
        g_state->asp_buf.ready = 0;
        state_exit(me);
        return;
    }

    Entity *e = &g_state->enemies[eidx];

    if (eidx >= 0 && eidx < 9) g_ready_tick_enemies[eidx] = -1;

    int action = timed_out ? g_cfg.enemy_skip : g_state->asp_buf.action;
    int target = timed_out ? 0 : g_state->asp_buf.target_index;

    g_state->asp_buf.ready = 0;
    g_state->asp_go = 0;

    if (timed_out) {
        snprintf(g_state->log.lines[g_state->log.head], LOG_LEN, "%s timed out -> skip", e->name);
        g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
    }

    if (action == g_cfg.enemy_strike) {
        if (target < 0) target = 0;
        if (target >= g_state->player_count) target = g_state->player_count - 1;

        Entity *p = &g_state->players[target];
        if (!p->alive) {
            // find first alive player
            for (int i = 0; i < g_state->player_count; i++) {
                if (g_state->players[i].alive) {
                    p = &g_state->players[i];
                    target = i;
                    break;
                }
            }
        }

        if (p->alive) {
            int dmg = e->damage;
            // If an enemy is holding a weapon, use its damage (Milestone 6/8)
            if (e->held_weapon >= 1 && e->held_weapon <= 8) {
                dmg = weapon_damage(e->held_weapon);

                int owner = owner_for_enemy(eidx);
                if (!res_try_lock_weapon(me, e->held_weapon, owner)) {
                    // Could not lock -> skip
                    snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                             "%s blocked: %s locked", e->name, weapon_name(e->held_weapon));
                    g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
                    e->stamina = e->max_stamina / 2;
                    state_exit(me);
                    return;
                }
            }

            p->hp -= dmg;
            p->hp = clamp_int(p->hp, 0, p->max_hp);

            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN, "%s strikes %s (-%d HP)", e->name, p->name, dmg);
            g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;

            if (e->held_weapon == 1 || e->held_weapon == 2) {
                int hip_pid = g_state->hip_pid;
                g_hip_stun_until_ms = now_ms() + 3000;
                p->stunned = 1;
                p->stun_end = (double)g_hip_stun_until_ms / 1000.0;
                send_stun_to_pid(hip_pid);
                snprintf(g_state->log.lines[g_state->log.head], LOG_LEN, "%s is stunned (3s)", p->name);
                g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
            }

            if ((e->held_weapon == 1 || e->held_weapon == 2) && (rand() % 10) < 2) {
                g_pending_ult_enemy[eidx] = 1;
                int other = (e->held_weapon == 1) ? 2 : 1;
                int owner = owner_for_enemy(eidx);
                (void)res_try_lock_weapon(me, e->held_weapon, owner);
                (void)res_try_lock_weapon(me, other, owner);
                snprintf(g_state->log.lines[g_state->log.head], LOG_LEN,
                         "%s attempts to acquire both artifacts...", e->name);
                g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
            }

            // Release lock if used
            if (e->held_weapon == 1 || e->held_weapon == 2) {
                int owner = owner_for_enemy(eidx);
                res_unlock_weapon(me, e->held_weapon, owner);
            }

            if (p->hp == 0) {
                p->alive = 0;
                if (target >= 0 && target < 4) g_ready_tick_players[target] = -1;
                snprintf(g_state->log.lines[g_state->log.head], LOG_LEN, "%s fell", p->name);
                g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;

                if (all_players_dead_unsafe()) {
                    g_state->phase = g_cfg.phase_gameover;
                }
            }
        }

        e->stamina = 0;
    } else {
        e->stamina = e->max_stamina / 2;
        if (!timed_out) {
            snprintf(g_state->log.lines[g_state->log.head], LOG_LEN, "%s skips (50%% stamina)", e->name);
            g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
        }
    }

    state_exit(me);
}

static int pick_next_actor_unsafe(int *out_is_player, int *out_idx)
{
    int best_tick = 1 << 30;
    int best_is_player = 1;
    int best_idx = -1;

    for (int i = 0; i < g_state->player_count; i++) {
        Entity *p = &g_state->players[i];
        if (!p->alive) continue;
        if (now_ms() < g_hip_stun_until_ms) continue;
        if (p->stamina < p->max_stamina) continue;
        int t = g_ready_tick_players[i];
        if (t < 0) continue;
        if (t < best_tick || (t == best_tick && (best_is_player == 0 || i < best_idx))) {
            best_tick = t;
            best_is_player = 1;
            best_idx = i;
        }
    }

    for (int i = 0; i < g_state->enemy_count; i++) {
        Entity *e = &g_state->enemies[i];
        if (!e->alive) continue;
        if (g_asp_suspended) continue;
        if (now_ms() < g_asp_stun_until_ms) continue;
        if (e->stamina < e->max_stamina) continue;
        int t = g_ready_tick_enemies[i];
        if (t < 0) continue;
        if (t < best_tick || (t == best_tick && best_is_player == 1)) {
            best_tick = t;
            best_is_player = 0;
            best_idx = i;
        }
    }

    if (best_idx < 0) return 0;
    *out_is_player = best_is_player;
    *out_idx = best_idx;
    return 1;
}

static void scheduler_step(int me, int seconds_tick)
{
    // seconds_tick=1 means "one simulated second" elapsed: add speed to stamina.
    state_enter(me);

    if (g_state->phase != g_cfg.phase_playing) {
        state_exit(me);
        return;
    }

    if (seconds_tick) {
        deadlock_check_and_resolve_unsafe(me);

        // Retry any blocked ultimate attempts opportunistically.
        try_complete_pending_ultimates_unsafe(me);

        // Handle delayed enemy respawns so deaths are visible.
        for (int i = 0; i < g_state->enemy_count; i++) {
            if (g_enemy_respawn_secs[i] > 0) {
                g_enemy_respawn_secs[i]--;
                if (g_enemy_respawn_secs[i] == 0) {
                    respawn_enemy_slot_unsafe(i);
                    snprintf(g_state->log.lines[g_state->log.head], LOG_LEN, "A new enemy appears!");
                    g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
                }
            }
        }

        for (int i = 0; i < g_state->player_count; i++) {
            Entity *p = &g_state->players[i];
            if (!p->alive) continue;
            int was_full = (p->stamina >= p->max_stamina);
            p->stamina += p->speed;
            if (p->stamina > p->max_stamina) p->stamina = p->max_stamina;
            if (!was_full && p->stamina >= p->max_stamina && g_ready_tick_players[i] < 0)
                g_ready_tick_players[i] = g_state->scheduler_tick;
        }
        // If ASP is suspended, do not increment enemy stamina; we will catch up on resume.
        if (!g_asp_suspended) {
        for (int i = 0; i < g_state->enemy_count; i++) {
            Entity *e = &g_state->enemies[i];
            if (!e->alive) continue;
            int was_full = (e->stamina >= e->max_stamina);
            e->stamina += e->speed;
            if (e->stamina > e->max_stamina) e->stamina = e->max_stamina;
            if (!was_full && e->stamina >= e->max_stamina && g_ready_tick_enemies[i] < 0)
                g_ready_tick_enemies[i] = g_state->scheduler_tick;
        }
        }
        g_state->scheduler_tick++;
    }

    // If not waiting on anyone, schedule next actor.
    if (!g_state->hip_go && !g_state->asp_go) {
        int is_player = 1;
        int idx = 0;
        if (pick_next_actor_unsafe(&is_player, &idx)) {
            g_state->active_is_player = is_player;
            g_state->active_index = idx;

            if (is_player) {
                g_state->hip_buf.ready = 0;
                g_state->hip_go = 1;
                snprintf(g_state->log.lines[g_state->log.head], LOG_LEN, "Turn: %s", g_state->players[idx].name);
            } else {
                if (g_asp_suspended || now_ms() < g_asp_stun_until_ms) {
                    // ASP cannot act right now.
                    state_exit(me);
                    return;
                }
                g_state->asp_buf.ready = 0;
                g_state->asp_go = 1;
                snprintf(g_state->log.lines[g_state->log.head], LOG_LEN, "Turn: %s", g_state->enemies[idx].name);
            }
            g_state->log.head = (g_state->log.head + 1) % LOG_COUNT;
        }
    }

    state_exit(me);
}

static int ui_pop_key(int me)
{
    int k = -1;
    thread_enter(me);
    if (g_ui_has_key) {
        k = (int)g_ui_key;
        g_ui_has_key = 0;
        g_ui_key = -1;
    }
    thread_exit(me);
    return k;
}

static void ui_push_key_from_render(int key)
{
    thread_enter(THREAD_REND);
    if (!g_ui_has_key) {
        g_ui_key = key;
        g_ui_has_key = 1;
    }
    thread_exit(THREAD_REND);
}

static void kbd_push_for_hip(int key)
{
    // Non-blocking input path: render thread publishes keys to a single-producer
    // ring buffer. HIP drains it as a single consumer.
    (void)kbd_push(&g_state->kbd, key);
}

static void draw_bar(sf::RenderWindow &w, float x, float y, float bw, float bh, float ratio,
                     sf::Color fill, sf::Color bg)
{
    auto snap = [](float v) -> float {
        return (float)((int)(v + 0.5f));
    };
    x = snap(x);
    y = snap(y);

    sf::RectangleShape back({bw, bh});
    back.setPosition(x, y);
    back.setFillColor(bg);
    w.draw(back);

    float fw = bw * ratio;
    if (fw < 0.f) fw = 0.f;
    if (fw > bw) fw = bw;
    sf::RectangleShape front({fw, bh});
    front.setPosition(x, y);
    front.setFillColor(fill);
    w.draw(front);

    sf::RectangleShape border({bw, bh});
    border.setPosition(x, y);
    border.setFillColor(sf::Color::Transparent);
    border.setOutlineThickness(1.f);
    border.setOutlineColor(sf::Color(80, 80, 110));
    w.draw(border);
}

static void render_loop(sf::RenderWindow *window)
{
    window->setActive(true);

    sf::Font font;
    font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");

    sf::Texture heroTex[4];
    bool heroTexOk[4];
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "assets/Hero_%d.png", i + 1);
        heroTexOk[i] = heroTex[i].loadFromFile(path);
        if (heroTexOk[i]) heroTex[i].setSmooth(false);
    }

    // NPC Textures (npc_1.png and npc_2.png)
    sf::Texture npcTex[2];
    bool npcTexOk[2];
    npcTexOk[0] = npcTex[0].loadFromFile("assets/npc_1.png");
    if (npcTexOk[0]) npcTex[0].setSmooth(false);
    npcTexOk[1] = npcTex[1].loadFromFile("assets/npc_2.png");
    if (npcTexOk[1]) npcTex[1].setSmooth(false);

    // Load Weapon Sprites (Expected: a 128x16 image containing 8 weapons side-by-side)
    sf::Texture weaponsTex;
    bool weaponsTexOk = weaponsTex.loadFromFile("assets/weapons.png");
    if (weaponsTexOk) weaponsTex.setSmooth(false);
    const int WEAPON_W = 16;
    const int WEAPON_H = 16;

    // Hero sheet: robot.png (6 frames of 32x32)
    // First 3 = Idle, Last 3 = Attack
    int HERO_FRAME_W = 32;
    int HERO_FRAME_H = 32;
    const int HERO_IDLE_FRAMES = 3;
    const int HERO_ATK_FRAMES  = 3;
    const int HERO_HEAL_FRAMES = 1;

    if (heroTexOk[0]) {
        sf::Vector2u sz = heroTex[0].getSize();
        if (sz.x >= 6 && (sz.x % 6) == 0 && sz.y > 0) {
            HERO_FRAME_W = (int)(sz.x / 6);
            HERO_FRAME_H = (int)sz.y;
        }
    }

    auto rect_for = [&](int frame_w, int frame_h, int row, int col) -> sf::IntRect {
        return sf::IntRect(col * frame_w, row * frame_h, frame_w, frame_h);
    };

    enum { AM_IDLE = 0, AM_ATTACK = 1, AM_HURT = 2, AM_HEAL = 3 };
    struct AnimState {
        int mode;
        long long until_ms;
        long long mode_start_ms;
        float xoff;
        float yoff;
    };
    static AnimState panim[4];
    static AnimState eanim[9];
    static int anim_inited = 0;
    if (!anim_inited) {
        for (int i = 0; i < 4; i++) { panim[i] = {AM_IDLE, 0, 0, 0.f, 0.f}; }
        for (int i = 0; i < 9; i++) { eanim[i] = {AM_IDLE, 0, 0, 0.f, 0.f}; }
        anim_inited = 1;
    }

    // --- Floating Damage Numbers ---
    struct DmgNum {
        float x, y;
        float vy;
        float alpha;
        char  text[16];
        sf::Color color;
        int   active;
    };
    static DmgNum dmg_nums[32];
    static int dmg_inited = 0;
    if (!dmg_inited) {
        for (int i = 0; i < 32; i++) dmg_nums[i].active = 0;
        dmg_inited = 1;
    }

    auto spawn_dmg = [&](float x, float y, int val, sf::Color col) {
        for (int i = 0; i < 32; i++) {
            if (!dmg_nums[i].active) {
                dmg_nums[i] = {x, y, -0.8f, 255.f, {}, col, 1};
                snprintf(dmg_nums[i].text, 16, "-%d", val);
                break;
            }
        }
    };

    // Help overlay toggle state
    static int show_help = 0;

    // Previous HP tracking for damage number spawning
    static int prev_hp_pl[4] = {-1,-1,-1,-1};
    static int prev_hp_en[9] = {-1,-1,-1,-1,-1,-1,-1,-1,-1};

    auto set_mode = [&](AnimState &a, int mode, int dur_ms) {
        a.mode = mode;
        a.mode_start_ms = now_ms();
        a.until_ms = a.mode_start_ms + dur_ms;
    };

    auto frame_index = [&](const AnimState &a, int fps, int frames) -> int {
        long long t = now_ms();
        long long dt = t - a.mode_start_ms;
        if (dt < 0) dt = 0;
        int f = (int)((dt * fps) / 1000LL);
        if (frames <= 0) return 0;
        return f % frames;
    };

    // Detect new log lines to trigger animations.
    static int prev_log_head = -1;
    static int prev_active_is_player = -1;
    static int prev_active_idx = -1;

    const sf::Color C_BG_TL(20, 16, 38);
    const sf::Color C_BG_TR(18, 26, 42);
    const sf::Color C_BG_BL(10, 12, 26);
    const sf::Color C_BG_BR(26, 18, 40);

    const sf::Color C_PANEL(40, 34, 66);
    const sf::Color C_PANEL_2(78, 68, 128);
    const sf::Color C_PANEL_3(30, 26, 50);
    const sf::Color C_TEXT(248, 248, 255);
    const sf::Color C_DIM(198, 196, 220);
    const sf::Color C_HP(242, 86, 118);
    const sf::Color C_STA(82, 232, 170);
    const sf::Color C_ACTIVE(255, 220, 110);
    const sf::Color C_ACCENT(90, 210, 235);

    sf::Clock frame;

    while (g_running && window->isOpen()) {
        sf::Event ev;
        while (window->pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) {
                g_running = 0;
                window->close();
            }
            if (ev.type == sf::Event::Resized) {
                // Keep view 1:1 with pixels (avoids unintended scaling blur)
                sf::View v(sf::FloatRect(0.f, 0.f, (float)ev.size.width, (float)ev.size.height));
                window->setView(v);
            }
            if (ev.type == sf::Event::KeyPressed) {
                int key = (int)ev.key.code;
                printf("[ARBITER DEBUG] SFML KeyPressed detected! key code = %d\n", key);

                // ESC toggles pause during battle
                if (key == (int)sf::Keyboard::Escape) {
                    state_enter(THREAD_REND);
                    int cur_phase = g_state->phase;
                    state_exit(THREAD_REND);
                    if (cur_phase == g_cfg.phase_playing) g_paused = !g_paused;
                }
                // F1 toggles help overlay
                if (key == (int)sf::Keyboard::F1) show_help = !show_help;

                ui_push_key_from_render(key);
                if (!g_paused) kbd_push_for_hip(key);
            }
        }

        (void)frame.restart();

        // Snapshot state for drawing
        int phase;
        int pc, ec, killed;
        int act_is_player, act_idx;
        int hip_go, asp_go;
        int hip_sel_target;
        Entity players[4];
        Entity enemies[9];
        ActionLog alog;
        DropState drop;
        int team[4];

        int scheduler_tick;

        state_enter(THREAD_REND);
        phase = g_state->phase;
        pc = g_state->player_count;
        ec = g_state->enemy_count;
        killed = g_state->enemies_killed;
        act_is_player = g_state->active_is_player;
        act_idx = g_state->active_index;
        hip_go = g_state->hip_go;
        asp_go = g_state->asp_go;
        hip_sel_target = g_state->hip_buf.target_index;
        drop = g_state->drop;
        alog = g_state->log;
        scheduler_tick = g_state->scheduler_tick;
        for (int i = 0; i < 4; i++) { players[i] = g_state->players[i]; team[i] = g_state->team[i]; }
        for (int i = 0; i < 9; i++) enemies[i] = g_state->enemies[i];
        int team_kills[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++) team_kills[i] = g_state->team_kills[i];
        state_exit(THREAD_REND);

        // Detect multiplayer: if any hero has team > 0
        int is_multiplayer = 0;
        for (int i = 0; i < 4; i++) if (team[i] > 0) { is_multiplayer = 1; break; }

        // Log-triggered animations
        if (prev_log_head < 0) prev_log_head = alog.head;
        if (alog.head != prev_log_head) {
            int last = alog.head - 1;
            if (last < 0) last += LOG_COUNT;
            const char *line = alog.lines[last];

            if (prev_active_idx >= 0) {
                if (prev_active_is_player == 1 && prev_active_idx < 4) {
                    if (strstr(line, "heals")) set_mode(panim[prev_active_idx], AM_HEAL, 500);
                    else if (strstr(line, "strikes") || strstr(line, "exhausts") || strstr(line, "uses"))
                        set_mode(panim[prev_active_idx], AM_ATTACK, 450);
                } else if (prev_active_is_player == 0 && prev_active_idx < 9) {
                    if (strstr(line, "strikes")) set_mode(eanim[prev_active_idx], AM_ATTACK, 450);
                }
            }

            // If someone fell/defeated, flash hurt on the currently targeted side.
            // (Simple heuristic; keeps visuals lively without extra shared state.)
            if (strstr(line, "(-") && strstr(line, "HP")) {
                // Hurt last target: if player acted, enemy was hurt; else player was hurt.
                if (prev_active_is_player == 1) {
                    for (int i = 0; i < ec && i < 9; i++) {
                        if (!enemies[i].alive) continue;
                    }
                } else {
                    for (int i = 0; i < pc && i < 4; i++) {
                        if (!players[i].alive) continue;
                    }
                }
            }

            prev_log_head = alog.head;
        }

        // Remember current active actor so next log line can animate it.
        prev_active_is_player = act_is_player;
        prev_active_idx = act_idx;

        sf::Vector2u sz = window->getSize();
        float W = (float)sz.x;
        float H = (float)sz.y;

        // Gradient background
        {
            sf::VertexArray quad(sf::TriangleStrip, 4);
            quad[0].position = sf::Vector2f(0.f, 0.f);
            quad[1].position = sf::Vector2f(W, 0.f);
            quad[2].position = sf::Vector2f(0.f, H);
            quad[3].position = sf::Vector2f(W, H);
            quad[0].color = C_BG_TL;
            quad[1].color = C_BG_TR;
            quad[2].color = C_BG_BL;
            quad[3].color = C_BG_BR;
            window->draw(quad);

            for (int i = 0; i < 60; i++) {
                float x = (float)((i * 97) % (int)W);
                float y = (float)((i * 57) % (int)H);
                if (y > H * 0.88f) continue; // keep the battle box clean
                sf::CircleShape dot(1.f);
                dot.setPosition(x, y);
                dot.setFillColor(sf::Color(255, 255, 255, (sf::Uint8)(40 + (i % 60))));
                window->draw(dot);
            }
        }

        auto draw_text = [&](const char *s, float x, float y, int size, sf::Color col) {
            auto snap = [](float v) -> float {
                return (float)((int)(v + 0.5f));
            };
            sf::Text t;
            t.setFont(font);
            t.setString(s);
            t.setCharacterSize(size);
            t.setFillColor(col);
            t.setPosition(snap(x), snap(y));
            (*window).draw(t);
        };

        if (phase == g_cfg.phase_start) {
            sf::RectangleShape panel({W * 0.70f, H * 0.35f});
            panel.setPosition(W * 0.15f, H * 0.30f);
            panel.setFillColor(sf::Color(C_PANEL.r, C_PANEL.g, C_PANEL.b, 235));
            panel.setOutlineThickness(2.f);
            panel.setOutlineColor(C_PANEL_2);
            window->draw(panel);

            sf::RectangleShape header({W * 0.70f, 44.f});
            header.setPosition(W * 0.15f, H * 0.30f);
            header.setFillColor(sf::Color(C_PANEL_3.r, C_PANEL_3.g, C_PANEL_3.b, 245));
            window->draw(header);

            sf::RectangleShape line({W * 0.70f, 2.f});
            line.setPosition(W * 0.15f, H * 0.30f + 44.f);
            line.setFillColor(C_ACCENT);
            window->draw(line);

            draw_text("CHRONO RIFT", W * 0.29f, H * 0.34f, 64, C_TEXT);
            draw_text("Press Enter to begin", W * 0.35f, H * 0.46f, 24, C_DIM);
        }
        else if (phase == g_cfg.phase_select) {
            sf::RectangleShape panel({W * 0.70f, H * 0.40f});
            panel.setPosition(W * 0.15f, H * 0.26f);
            panel.setFillColor(sf::Color(C_PANEL.r, C_PANEL.g, C_PANEL.b, 235));
            panel.setOutlineThickness(2.f);
            panel.setOutlineColor(C_PANEL_2);
            window->draw(panel);

            sf::RectangleShape header({W * 0.70f, 44.f});
            header.setPosition(W * 0.15f, H * 0.26f);
            header.setFillColor(sf::Color(C_PANEL_3.r, C_PANEL_3.g, C_PANEL_3.b, 245));
            window->draw(header);

            sf::RectangleShape line({W * 0.70f, 2.f});
            line.setPosition(W * 0.15f, H * 0.26f + 44.f);
            line.setFillColor(C_ACCENT);
            window->draw(line);

            draw_text("Select party size (1-4)", W * 0.29f, H * 0.30f, 34, C_TEXT);
            draw_text("Press Enter to confirm", W * 0.34f, H * 0.39f, 20, C_DIM);

            char buf[64];
            snprintf(buf, sizeof(buf), "Current selection: %d", (pc <= 0 ? 1 : pc));
            draw_text(buf, W * 0.39f, H * 0.50f, 22, C_ACTIVE);
        }
        else {
            float battle_ui_h = (H * 0.35f);
            if (battle_ui_h < 340.f) battle_ui_h = 340.f;
            if (battle_ui_h > 400.f) battle_ui_h = 400.f;

            float stage_top = 60.f;
            float stage_bottom = H - battle_ui_h - 12.f;

            // Top thin bar (simple)
            float top_h = (H * 0.06f);
            if (top_h < 42.f) top_h = 42.f;
            if (top_h > 56.f) top_h = 56.f;
            sf::RectangleShape top({W, top_h});
            top.setPosition(0.f, 0.f);
            top.setFillColor(C_PANEL_3);
            top.setOutlineThickness(0.f);
            window->draw(top);

            sf::RectangleShape top_line({W, 2.f});
            top_line.setPosition(0.f, top_h);
            top_line.setFillColor(C_ACCENT);
            window->draw(top_line);

            char kbuf[64];
            snprintf(kbuf, sizeof(kbuf), "Kills: %d / %d", killed, g_cfg.win_kill_count);
            draw_text(kbuf, 12.f, 10.f, 16, C_TEXT);

            // Multiplayer: show which team's turn it is
            if (is_multiplayer && act_is_player && hip_go && act_idx >= 0 && act_idx < 4) {
                int act_team = team[act_idx];
                sf::Color tcol = (act_team == 1) ? sf::Color(100, 220, 255) : sf::Color(255, 180, 80);
                char tbuf[32];
                snprintf(tbuf, sizeof(tbuf), "Player %d's Turn", act_team);
                draw_text(tbuf, W * 0.42f, 10.f, 18, tcol);
            }

            if (weaponsTexOk) {
                float grid_x = 24.f;
                float grid_y = top_h + 16.f;
                
                sf::RectangleShape bg({200.f, 104.f});
                bg.setPosition(grid_x, grid_y);
                bg.setFillColor(sf::Color(C_PANEL.r, C_PANEL.g, C_PANEL.b, 235));
                bg.setOutlineThickness(2.f);
                bg.setOutlineColor(C_PANEL_2);
                window->draw(bg);

                sf::RectangleShape bg_h({200.f, 24.f});
                bg_h.setPosition(grid_x, grid_y);
                bg_h.setFillColor(sf::Color(C_PANEL_3.r, C_PANEL_3.g, C_PANEL_3.b, 245));
                window->draw(bg_h);

                draw_text("Arsenal", grid_x + 12.f, grid_y + 2.f, 18, C_TEXT);

                int last_used = 0;
                if (act_is_player && act_idx >= 0 && act_idx < pc) {
                    last_used = g_ui_last_weapon[act_idx];
                }

                for (int w = 1; w <= 8; w++) {
                    int col = (w - 1) % 4;
                    int row = (w - 1) / 4;
                    float icon_x = grid_x + 16.f + col * 44.f;
                    float icon_y = grid_y + 30.f + row * 36.f;

                    // Draw the square frame for the weapon
                    sf::RectangleShape frame({32.f, 32.f});
                    frame.setPosition(icon_x, icon_y);
                    frame.setFillColor(sf::Color(0, 0, 0, 150));
                    
                    // Highlight the specific weapon used by the active player in YELLOW
                    if (w == last_used) {
                        frame.setOutlineThickness(2.f);
                        frame.setOutlineColor(sf::Color::Yellow);
                    } else {
                        frame.setOutlineThickness(1.f);
                        frame.setOutlineColor(sf::Color(80, 80, 80));
                    }
                    window->draw(frame);

                    sf::Sprite w_sprite(weaponsTex);
                    w_sprite.setTextureRect(sf::IntRect((w - 1) * WEAPON_W, 0, WEAPON_W, WEAPON_H));
                    w_sprite.setScale(2.f, 2.f); // Scale sprite bigger! 16x16 -> 32x32
                    w_sprite.setPosition(icon_x, icon_y);
                    window->draw(w_sprite);
                }
            }
            {
                float panel_w = W * 0.28f;
                if (panel_w < 280.f) panel_w = 280.f;
                if (panel_w > 360.f) panel_w = 360.f;

                float row_h = 44.f;
                int rows = ec;
                if (rows < 1) rows = 1;
                if (rows > 9) rows = 9;
                float panel_h = 12.f + (float)rows * row_h;
                float panel_x = W - panel_w - 12.f;
                float panel_y = 56.f;

                sf::RectangleShape ep({panel_w, panel_h});
                ep.setPosition(panel_x, panel_y);
                ep.setFillColor(sf::Color(C_PANEL.r, C_PANEL.g, C_PANEL.b, 235));
                ep.setOutlineThickness(2.f);
                ep.setOutlineColor(C_PANEL_2);
                window->draw(ep);

                sf::RectangleShape eh({panel_w, 28.f});
                eh.setPosition(panel_x, panel_y);
                eh.setFillColor(sf::Color(C_PANEL_3.r, C_PANEL_3.g, C_PANEL_3.b, 245));
                window->draw(eh);

                sf::RectangleShape el({panel_w, 2.f});
                el.setPosition(panel_x, panel_y + 28.f);
                el.setFillColor(C_ACCENT);
                window->draw(el);

                draw_text("Enemies", panel_x + 12.f, panel_y + 4.f, 18, C_TEXT);

                float ry = panel_y + 36.f;
                for (int i = 0; i < ec && i < 9; i++) {
                    const Entity &e = enemies[i];
                    sf::Color name_col = (!act_is_player && asp_go && act_idx == i) ? C_ACTIVE : C_TEXT;
                    if (!e.alive) name_col = C_DIM;

                    // Row background stripes + selection highlight
                    if (act_is_player && hip_go && i == hip_sel_target) {
                        sf::RectangleShape sel({panel_w - 6.f, row_h});
                        sel.setPosition(panel_x + 3.f, ry - 6.f);
                        sel.setFillColor(sf::Color(C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 26));
                        window->draw(sel);
                    } else if ((i % 2) == 0) {
                        sf::RectangleShape rr({panel_w - 6.f, row_h});
                        rr.setPosition(panel_x + 3.f, ry - 6.f);
                        rr.setFillColor(sf::Color(0, 0, 0, 18));
                        window->draw(rr);
                    }

                    char nbuf[96];
                    snprintf(nbuf, sizeof(nbuf), "%d) %s  %d/%d", i + 1, e.name, e.hp, e.max_hp);
                    draw_text(nbuf, panel_x + 12.f, ry, 14, name_col);

                    float hp_ratio = (e.max_hp > 0) ? (float)e.hp / (float)e.max_hp : 0.f;
                    float st_ratio = (e.max_stamina > 0) ? (float)e.stamina / (float)e.max_stamina : 0.f;
                    draw_bar(*window, panel_x + 12.f, ry + 18.f, panel_w - 24.f, 9.f, hp_ratio, C_HP, sf::Color(55, 40, 45));
                    draw_bar(*window, panel_x + 12.f, ry + 30.f, panel_w - 24.f, 9.f, st_ratio, C_STA, sf::Color(35, 55, 45));
                    ry += row_h;
                }
            }

            // Bottom battle box
            sf::RectangleShape box({W - 24.f, battle_ui_h});
            box.setPosition(12.f, H - battle_ui_h - 12.f);
            box.setFillColor(sf::Color(C_PANEL.r, C_PANEL.g, C_PANEL.b, 235));
            box.setOutlineThickness(2.f);
            box.setOutlineColor(C_PANEL_2);
            window->draw(box);

            // Split into left message/enemy area and right party area
            float box_x = 12.f;
            float box_y = H - battle_ui_h - 12.f;
            float box_w = W - 24.f;
            float left_w = box_w * 0.58f;
            float right_w = box_w - left_w;

            sf::RectangleShape divider({2.f, battle_ui_h - 20.f});
            divider.setPosition(box_x + left_w, box_y + 10.f);
            divider.setFillColor(sf::Color(70, 66, 105));
            window->draw(divider);

            // Enemy/turn label (left-top)
            char eb[96];
            if (!act_is_player && act_idx >= 0 && act_idx < ec)
                snprintf(eb, sizeof(eb), "Enemy: %s", enemies[act_idx].name);
            else if (act_is_player && act_idx >= 0 && act_idx < pc)
                snprintf(eb, sizeof(eb), "Turn: %s", players[act_idx].name);
            else
                snprintf(eb, sizeof(eb), "Battle");
            draw_text(eb, box_x + 14.f, box_y + 10.f, 18, C_TEXT);

            // Controls hint (small)
            if (drop.active) {
                draw_text("DROP: P=Pick up | L=Leave", box_x + 14.f, box_y + 34.f, 14, C_ACTIVE);
            } else {
                draw_text("A Strike | E Exhaust | H Heal | W Weapon | I SwapIn | U Ultimate | S Skip | Q Quit",
                          box_x + 14.f, box_y + 34.f, 13, C_DIM);
            }

            // Action log in left panel
            draw_text("Log", box_x + 14.f, box_y + 58.f, 16, C_TEXT);
            int start = alog.head;
            float ly = box_y + 78.f;
            for (int i = 0; i < LOG_COUNT; i++) {
                int idx = (start + i) % LOG_COUNT;
                if (alog.lines[idx][0] == '\0') continue;
                draw_text(alog.lines[idx], box_x + 14.f, ly, 14, C_DIM);
                ly += 16.f;
                if (ly > box_y + battle_ui_h - 18.f) break;
            }

            // Party status in right panel (HP/STA)
            float px = box_x + left_w + 12.f;
            float py = box_y + 10.f;
            draw_text("Party", px + 10.f, py, 18, C_TEXT);
            py += 26.f;
            for (int i = 0; i < pc; i++) {
                const Entity &p = players[i];
                // Multiplayer: color heroes by team
                sf::Color team1_col(100, 220, 255);  // Cyan
                sf::Color team2_col(255, 180, 80);   // Orange
                sf::Color name_col = (act_is_player && act_idx == i && hip_go) ? C_ACTIVE : C_TEXT;
                if (is_multiplayer && team[i] == 1) name_col = (act_is_player && act_idx == i && hip_go) ? C_ACTIVE : team1_col;
                if (is_multiplayer && team[i] == 2) name_col = (act_is_player && act_idx == i && hip_go) ? C_ACTIVE : team2_col;
                char nbuf[96];
                const char *tag = "";
                if (is_multiplayer) tag = (team[i] == 1) ? "[P1] " : "[P2] ";
                snprintf(nbuf, sizeof(nbuf), "%s%s  %d/%d", tag, p.name, p.hp, p.max_hp);
                draw_text(nbuf, px + 10.f, py, 15, p.alive ? name_col : C_DIM);
                float hp_ratio = (p.max_hp > 0) ? (float)p.hp / (float)p.max_hp : 0.f;
                float st_ratio = (p.max_stamina > 0) ? (float)p.stamina / (float)p.max_stamina : 0.f;
                draw_bar(*window, px + 10.f, py + 18.f, right_w - 26.f, 7.f, hp_ratio, C_HP, sf::Color(50, 30, 30));
                draw_bar(*window, px + 10.f, py + 28.f, right_w - 26.f, 7.f, st_ratio, C_STA, sf::Color(25, 50, 35));
                
                char inv_buf[128] = "Inv: ";
                int has_any = 0;
                float weapon_x = px + 10.f;
                
                for (int w = 1; w <= 8; w++) {
                    int has = 0;
                    for (int j = 0; j < 20; j++) {
                        if (p.inventory[j] == w) { has = 1; break; }
                    }
                    if (has) {
                        if (has_any) strncat(inv_buf, ", ", sizeof(inv_buf) - strlen(inv_buf) - 1);
                        char tmp[32];
                        snprintf(tmp, sizeof(tmp), "[%d] %s", w, weapon_name(w));
                        strncat(inv_buf, tmp, sizeof(inv_buf) - strlen(inv_buf) - 1);
                        has_any = 1;
                        
                        // Draw the specific weapon sprite if loaded
                        if (weaponsTexOk) {
                            sf::Sprite w_sprite(weaponsTex);
                            w_sprite.setScale(1.2f, 1.2f);
                            // X position advances based on the number of weapons we've drawn
                            w_sprite.setPosition(weapon_x, py + 50.f);
                            
                            // Map weapon_id to index 0..7
                            int col = w - 1;
                            w_sprite.setTextureRect(sf::IntRect(col * WEAPON_W, 0, WEAPON_W, WEAPON_H));
                            
                            window->draw(w_sprite);
                            weapon_x += 24.f; // Space between icons
                        }
                    }
                }
                if (!has_any) {
                    strncat(inv_buf, "None", sizeof(inv_buf) - strlen(inv_buf) - 1);
                }
                draw_text(inv_buf, px + 10.f, py + 38.f, 10, sf::Color(180, 180, 180));

                py += 75.f; // Increased spacing to fit the sprites
            }

            // Sprite positions (simple stage)
            float stage_y = stage_top + (stage_bottom - stage_top) * 0.58f;
            float hero_x0 = W * 0.25f;
            float enemy_x0 = W * 0.75f;
            float sprite_scale = (H / 720.f) * 1.7f;
            if (sprite_scale < 1.4f) sprite_scale = 1.4f;
            if (sprite_scale > 1.8f) sprite_scale = 1.8f;
            float hero_dx = 44.f * (sprite_scale / 1.7f);
            float enemy_dx = 44.f * (sprite_scale / 1.7f);

            // Animate/move active attacker slightly forward during attack.
            auto update_offsets = [&](AnimState &a, int is_hero, int is_active) {
                long long t = now_ms();
                if (a.until_ms > 0 && t > a.until_ms) {
                    a.mode = AM_IDLE;
                    a.until_ms = 0;
                    a.mode_start_ms = t;
                    a.xoff = 0.f;
                    a.yoff = 0.f;
                }

                if (a.mode == AM_ATTACK) {
                    // Step forward then back over 0.45s
                    float dur = 450.f;
                    float dt = (float)(t - a.mode_start_ms);
                    if (dt < 0.f) dt = 0.f;
                    if (dt > dur) dt = dur;
                    float half = dur * 0.5f;
                    float prog = (dt <= half) ? (dt / half) : (1.f - (dt - half) / half);
                    float dir = is_hero ? 1.f : -1.f;
                    a.xoff = dir * (18.f * prog);
                    a.yoff = -2.f * prog;
                } else if (a.mode == AM_HEAL) {
                    a.xoff = 0.f;
                    a.yoff = -2.f;
                } else {
                    a.xoff = 0.f;
                    a.yoff = 0.f;
                }

                // Idle bob for active entity while waiting
                if (a.mode == AM_IDLE && is_active) {
                    float bob = (float)((scheduler_tick % 20) - 10);
                    a.yoff = (bob > 0 ? -1.f : 0.f);
                }
            };

            // Draw hero sprites
            for (int i = 0; i < pc && i < 4; i++) {
                int is_active = (act_is_player && hip_go && act_idx == i);
                update_offsets(panim[i], 1, is_active);

                auto snap = [](float v) -> float { return (float)((int)(v + 0.5f)); };
                float x = snap(hero_x0 - (float)i * hero_dx + panim[i].xoff);
                float y = snap(stage_y + (float)i * 26.f + panim[i].yoff);

                if (heroTexOk[i]) {
                    // Hero sheet is a single row: attack frames are columns 0..5.
                    int row = 0;
                    int col = 0;
                    if (panim[i].mode == AM_ATTACK) { col = 3 + frame_index(panim[i], 12, HERO_ATK_FRAMES); }
                    else if (panim[i].mode == AM_HEAL) { col = frame_index(panim[i], 10, HERO_HEAL_FRAMES); }
                    else { col = frame_index(panim[i], 6, HERO_IDLE_FRAMES); }

                    sf::Sprite sp(heroTex[i]);
                    sp.setTextureRect(rect_for(HERO_FRAME_W, HERO_FRAME_H, row, col));
                    sp.setPosition(x, y);
                    sp.setScale(sprite_scale, sprite_scale);
                    if (!players[i].alive) sp.setColor(sf::Color(120, 120, 120));
                    window->draw(sp);
                } else {
                    sf::RectangleShape r({24.f, 24.f});
                    r.setPosition(x, y);
                    r.setFillColor(players[i].alive ? sf::Color(80, 160, 240) : sf::Color(80, 80, 80));
                    window->draw(r);
                }
            }

            // Draw enemy sprites
            for (int i = 0; i < ec && i < 9; i++) {
                int is_active = (!act_is_player && asp_go && act_idx == i);
                update_offsets(eanim[i], 0, is_active);

                auto snap = [](float v) -> float { return (float)((int)(v + 0.5f)); };
                float x = snap(enemy_x0 + (float)i * enemy_dx + eanim[i].xoff);
                float y = snap(stage_y + (float)i * 18.f + eanim[i].yoff);

                int tex_idx = i % 2;
                if (npcTexOk[tex_idx]) {
                    // Enemies reuse the same sheet format for now.
                    int erow = 0;
                    int ecol = 0;
                    if (eanim[i].mode == AM_ATTACK) {
                        ecol = 3 + frame_index(eanim[i], 12, HERO_ATK_FRAMES);
                    } else {
                        ecol = frame_index(eanim[i], 6, HERO_IDLE_FRAMES);
                    }

                    sf::Sprite sp(npcTex[tex_idx]);
                    sp.setTextureRect(rect_for(HERO_FRAME_W, HERO_FRAME_H, erow, ecol));
                    sp.setPosition(x, y);
                    sp.setScale(sprite_scale, sprite_scale);
                    if (!enemies[i].alive) sp.setColor(sf::Color(120, 120, 120));
                    sp.setScale(-sprite_scale, sprite_scale); // face left
                    sp.setOrigin((float)HERO_FRAME_W, 0.f);
                    window->draw(sp);
                } else {
                    sf::RectangleShape r({24.f, 24.f});
                    r.setPosition(x, y);
                    r.setFillColor(enemies[i].alive ? sf::Color(240, 120, 120) : sf::Color(80, 80, 80));
                    window->draw(r);
                }
            }

            // --- Spawn floating damage numbers on HP change ---
            for (int i = 0; i < pc && i < 4; i++) {
                if (prev_hp_pl[i] >= 0 && players[i].hp < prev_hp_pl[i] && players[i].alive)
                    spawn_dmg(W * 0.25f - (float)i * 44.f, stage_y + (float)i * 26.f - 36.f,
                              prev_hp_pl[i] - players[i].hp, C_HP);
                prev_hp_pl[i] = players[i].hp;
            }
            for (int i = 0; i < ec && i < 9; i++) {
                if (prev_hp_en[i] >= 0 && enemies[i].hp < prev_hp_en[i] && enemies[i].alive)
                    spawn_dmg(W * 0.75f + (float)i * 44.f, stage_y + (float)i * 18.f - 36.f,
                              prev_hp_en[i] - enemies[i].hp, C_ACTIVE);
                if (enemies[i].alive) prev_hp_en[i] = enemies[i].hp;
            }

            // --- Tick and draw floating damage numbers ---
            for (int i = 0; i < 32; i++) {
                if (!dmg_nums[i].active) continue;
                dmg_nums[i].y += dmg_nums[i].vy;
                dmg_nums[i].alpha -= 2.5f;
                if (dmg_nums[i].alpha <= 0.f) { dmg_nums[i].active = 0; continue; }
                sf::Color c = dmg_nums[i].color;
                c.a = (sf::Uint8)dmg_nums[i].alpha;
                draw_text(dmg_nums[i].text, dmg_nums[i].x, dmg_nums[i].y, 20, c);
            }

            // --- PAUSE overlay ---
            if (g_paused) {
                sf::RectangleShape dim({W, H});
                dim.setPosition(0.f, 0.f);
                dim.setFillColor(sf::Color(0, 0, 0, 160));
                window->draw(dim);

                float pw = 340.f, ph = 180.f;
                float ppx = (W - pw) * 0.5f, ppy = (H - ph) * 0.5f;
                sf::RectangleShape panel({pw, ph});
                panel.setPosition(ppx, ppy);
                panel.setFillColor(sf::Color(22, 18, 44, 245));
                panel.setOutlineThickness(2.f);
                panel.setOutlineColor(sf::Color(90, 210, 235));
                window->draw(panel);
                draw_text("PAUSED",         ppx + 108.f, ppy + 20.f,  32, sf::Color(90, 210, 235));
                draw_text("[ESC]  Resume",  ppx +  90.f, ppy + 80.f,  20, sf::Color(220, 220, 220));
                draw_text("[Q]    Quit",    ppx +  90.f, ppy + 112.f, 20, sf::Color(220, 100, 100));
            }

            // --- VICTORY SCREEN ---
            if (phase == g_cfg.phase_win) {
                sf::RectangleShape dim({W, H});
                dim.setPosition(0.f, 0.f);
                dim.setFillColor(sf::Color(0, 0, 0, 185));
                window->draw(dim);

                float pw = 460.f, ph = 290.f;
                float ppx = (W - pw) * 0.5f, ppy = (H - ph) * 0.5f;
                sf::RectangleShape panel({pw, ph});
                panel.setPosition(ppx, ppy);
                panel.setFillColor(sf::Color(16, 36, 24, 248));
                panel.setOutlineThickness(3.f);
                panel.setOutlineColor(sf::Color(82, 232, 170));
                window->draw(panel);

                draw_text("VICTORY!", ppx + 120.f, ppy + 16.f, 44, sf::Color(255, 220, 110));

                if (is_multiplayer) {
                    sf::Color t1_c(100, 220, 255), t2_c(255, 180, 80);
                    if (team_kills[1] > team_kills[2])
                        draw_text("TEAM 1 WINS!", ppx + 130.f, ppy + 65.f, 24, t1_c);
                    else if (team_kills[2] > team_kills[1])
                        draw_text("TEAM 2 WINS!", ppx + 130.f, ppy + 65.f, 24, t2_c);
                    else
                        draw_text("IT'S A DRAW!", ppx + 140.f, ppy + 65.f, 24, sf::Color::White);

                    char tk1[32], tk2[32];
                    snprintf(tk1, sizeof(tk1), "Team 1 Kills: %d", team_kills[1]);
                    snprintf(tk2, sizeof(tk2), "Team 2 Kills: %d", team_kills[2]);
                    draw_text(tk1, ppx + 60.f, ppy + 100.f, 18, t1_c);
                    draw_text(tk2, ppx + 60.f, ppy + 125.f, 18, t2_c);
                }

                int alive_count = 0;
                for (int i = 0; i < pc; i++) if (players[i].alive) alive_count++;
                long long elapsed_s = (g_game_start_ms > 0) ? (now_ms() - g_game_start_ms) / 1000LL : 0;

                char s1[64], s2[64], s3[64];
                snprintf(s1, sizeof(s1), "Total Kills    : %d", killed);
                snprintf(s2, sizeof(s2), "Heroes Standing: %d / %d", alive_count, pc);
                snprintf(s3, sizeof(s3), "Time Played    : %llds", elapsed_s);

                float start_y = is_multiplayer ? 155.f : 96.f;
                draw_text(s1, ppx + 60.f, ppy + start_y, 20, sf::Color(190, 220, 190));
                draw_text(s2, ppx + 60.f, ppy + start_y + 30.f, 20, sf::Color(190, 220, 190));
                draw_text(s3, ppx + 60.f, ppy + start_y + 60.f, 20, sf::Color(190, 220, 190));
                draw_text("Press Enter to Exit", ppx + 96.f, ppy + 232.f, 18, sf::Color(130, 130, 130));
            }

            // --- GAME OVER SCREEN ---
            if (phase == g_cfg.phase_gameover) {
                sf::RectangleShape dim({W, H});
                dim.setPosition(0.f, 0.f);
                dim.setFillColor(sf::Color(0, 0, 0, 195));
                window->draw(dim);

                float pw = 460.f, ph = 260.f;
                float ppx = (W - pw) * 0.5f, ppy = (H - ph) * 0.5f;
                sf::RectangleShape panel({pw, ph});
                panel.setPosition(ppx, ppy);
                panel.setFillColor(sf::Color(36, 12, 12, 248));
                panel.setOutlineThickness(3.f);
                panel.setOutlineColor(sf::Color(232, 82, 82));
                window->draw(panel);

                draw_text("GAME OVER", ppx + 100.f, ppy + 16.f, 44, sf::Color(232, 82, 82));

                if (is_multiplayer) {
                    draw_text("BOTH TEAMS DEFEATED", ppx + 95.f, ppy + 65.f, 22, sf::Color(255, 150, 150));
                    char sk1[64], sk2[64];
                    snprintf(sk1, sizeof(sk1), "Team 1 Kills: %d", team_kills[1]);
                    snprintf(sk2, sizeof(sk2), "Team 2 Kills: %d", team_kills[2]);
                    draw_text(sk1, ppx + 60.f, ppy + 95.f, 18, sf::Color(100, 220, 255));
                    draw_text(sk2, ppx + 60.f, ppy + 120.f, 18, sf::Color(255, 180, 80));
                }

                long long elapsed_s = (g_game_start_ms > 0) ? (now_ms() - g_game_start_ms) / 1000LL : 0;
                char s1[64], s2[64];
                snprintf(s1, sizeof(s1), "Total Kills     : %d", killed);
                snprintf(s2, sizeof(s2), "Time Survived   : %llds", elapsed_s);

                float gstart_y = is_multiplayer ? 150.f : 100.f;
                draw_text(s1, ppx + 60.f, ppy + gstart_y, 20, sf::Color(220, 180, 180));
                draw_text(s2, ppx + 60.f, ppy + gstart_y + 32.f, 20, sf::Color(220, 180, 180));
                draw_text("Press Enter to Exit", ppx + 96.f, ppy + 206.f, 18, sf::Color(130, 130, 130));
            }
        }

        if (show_help) {
            sf::Vector2u sz2 = window->getSize();
            float W2 = (float)sz2.x;
            float hw = 380.f, hh = 292.f;
            float hx = W2 - hw - 18.f, hy = 66.f;
            sf::RectangleShape hbg({hw, hh});
            hbg.setPosition(hx, hy);
            hbg.setFillColor(sf::Color(10, 8, 26, 242));
            hbg.setOutlineThickness(2.f);
            hbg.setOutlineColor(sf::Color(90, 210, 235));
            window->draw(hbg);
            draw_text("Controls  [F1 to hide]", hx + 14.f, hy + 8.f, 16, sf::Color(90, 210, 235));
            const char *help[] = {
                "A          Attack (Strike)",
                "E          Attack (Exhaust stamina)",
                "H          Heal (+10% HP)",
                "W + 1-8    Use Weapon",
                "I + 1-8    Swap In from storage",
                "U          Ultimate Ability",
                "S          Skip turn",
                "1-9        Select enemy target",
                "Q          Quit game",
                "ESC        Pause / Resume",
                "F1         Toggle this help",
            };
            float hl_y = hy + 34.f;
            for (int i = 0; i < 11; i++) {
                sf::Color lc = (i % 2 == 0) ? sf::Color(200, 200, 200) : sf::Color(165, 165, 200);
                draw_text(help[i], hx + 16.f, hl_y, 15, lc);
                hl_y += 22.f;
            }
        }

        window->display();
    }
}

int main()
{
    load_config(&g_cfg, "config.txt");

    // Milestone 7/9: sigaction usage for correctness
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = handle_sigterm;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, NULL);

    struct sigaction sa_alrm;
    memset(&sa_alrm, 0, sizeof(sa_alrm));
    sa_alrm.sa_handler = handle_sigalrm;
    sigemptyset(&sa_alrm.sa_mask);
    sa_alrm.sa_flags = 0;
    sigaction(SIGALRM, &sa_alrm, NULL);

    // Init thread lock
    g_thread_lock.interested[0] = 0;
    g_thread_lock.interested[1] = 0;
    g_thread_lock.interested[2] = 0;
    g_thread_lock.turn = 0;

    XInitThreads();

    shm_create();

    // Create window in scheduler thread, render in dedicated thread
    sf::VideoMode dm = sf::VideoMode::getDesktopMode();
    unsigned int ww = 1920;
    unsigned int wh = 1080;
    if (ww > dm.width) ww = dm.width;
    if (wh > dm.height) wh = dm.height;

    sf::RenderWindow window(sf::VideoMode(ww, wh), "Chrono Rift", sf::Style::Titlebar | sf::Style::Close);
    // Center window on screen
    window.setPosition(sf::Vector2i((int)(dm.width - ww) / 2, (int)(dm.height - wh) / 2));
    window.setVerticalSyncEnabled(true);
    window.setFramerateLimit(60);
    window.setActive(false);

    std::thread rt(render_loop, &window);

    int selected_party = 1;

    // Main scheduler loop
    int ticks_per_sec = (g_cfg.tick_ms > 0) ? (1000 / g_cfg.tick_ms) : 10;
    if (ticks_per_sec <= 0) ticks_per_sec = 10;
    int tick_count = 0;

    while (g_running) {
        // Phase handling based on Arbiter UI keys
        int key = ui_pop_key(THREAD_SCHED);

        state_enter(THREAD_SCHED);
        int phase = g_state->phase;
        state_exit(THREAD_SCHED);

        if (phase == g_cfg.phase_start) {
            if (key == (int)sf::Keyboard::Enter) {
                state_enter(THREAD_SCHED);
                g_state->phase = g_cfg.phase_select;
                g_state->player_count = selected_party;
                state_exit(THREAD_SCHED);
            }
            sleep_ms(20);
            continue;
        }

        if (phase == g_cfg.phase_select) {
            state_enter(THREAD_SCHED);
            int mp_detected = 0;
            for (int i = 0; i < 4; i++) if (g_state->team[i] > 0) { mp_detected = 1; break; }
            state_exit(THREAD_SCHED);

            if (mp_detected) {
                selected_party = 4;
                init_game_state(THREAD_SCHED, selected_party);
                g_game_start_ms = now_ms();
                tick_count = 0;
                printf("[ARBITER] Multiplayer detected! Auto-starting with 4 heroes.\n");
                sleep_ms(20);
                continue;
            }

            if (key == (int)sf::Keyboard::Num1) selected_party = 1;
            if (key == (int)sf::Keyboard::Num2) selected_party = 2;
            if (key == (int)sf::Keyboard::Num3) selected_party = 3;
            if (key == (int)sf::Keyboard::Num4) selected_party = 4;

            if (selected_party < 1) selected_party = 1;
            if (selected_party > g_cfg.max_players) selected_party = g_cfg.max_players;

            state_enter(THREAD_SCHED);
            g_state->player_count = selected_party;
            state_exit(THREAD_SCHED);

            if (key == (int)sf::Keyboard::Enter) {
                init_game_state(THREAD_SCHED, selected_party);
                g_game_start_ms = now_ms(); // start the clock
                tick_count = 0;
            }

            sleep_ms(20);
            continue;
        }

        // PLAYING/WIN/GAMEOVER loop
        if (phase == g_cfg.phase_playing) {
            sleep_ms(g_cfg.tick_ms);

            // If Ultimate alarm fired, resume ASP (and catch-up staminas)
            resume_asp_if_alarm(THREAD_SCHED);

            tick_count++;
            int seconds_tick = 0;
            if (tick_count >= ticks_per_sec) {
                tick_count = 0;
                seconds_tick = 1;
            }

            scheduler_step(THREAD_SCHED, seconds_tick);

            // If we are waiting on actions, check buffers
            state_enter(THREAD_SCHED);
            int hip_go = g_state->hip_go;
            int asp_go = g_state->asp_go;
            int act_is_pl = g_state->active_is_player;
            int hip_ready = g_state->hip_buf.ready;
            int asp_ready = g_state->asp_buf.ready;
            state_exit(THREAD_SCHED);

            // Player timeout: prevents indefinite freeze if no input arrives.
            static int hip_wait_ticks = 0;
            if (hip_go && act_is_pl && !hip_ready) {
                hip_wait_ticks++;
                int limit = g_cfg.hip_timeout_ticks;
                if (limit <= 0) limit = 0;
                if (limit > 0 && hip_wait_ticks >= limit) {
                    // Auto-skip
                    state_enter(THREAD_SCHED);
                    int drop_active = g_state->drop.active;
                    g_state->hip_buf.action = g_cfg.action_skip;
                    g_state->hip_buf.target_index = 0;
                    g_state->hip_buf.aux = 0; // 0 means Leave for drops
                    g_state->hip_buf.ready = 1;
                    state_exit(THREAD_SCHED);
                    
                    hip_wait_ticks = 0;
                    
                    if (drop_active) {
                        log_actionf(THREAD_SCHED, "Player drop prompt timeout -> leave");
                        apply_drop_decision(THREAD_SCHED);
                    } else {
                        log_actionf(THREAD_SCHED, "Player input timeout -> skip");
                        apply_player_action(THREAD_SCHED);
                    }
                }
            } else {
                hip_wait_ticks = 0;
            }

            if (hip_go && act_is_pl && hip_ready) {
                hip_wait_ticks = 0;
                // If a weapon drop prompt is active, the same player answers with hip_buf.aux.
                state_enter(THREAD_SCHED);
                int drop_active = g_state->drop.active;
                int drop_for = g_state->drop.for_player;
                int active_idx = g_state->active_index;
                state_exit(THREAD_SCHED);

                if (drop_active && drop_for == active_idx) {
                    apply_drop_decision(THREAD_SCHED);
                } else {
                    apply_player_action(THREAD_SCHED);
                }
            }

            if (asp_go && !act_is_pl) {
                static int npc_wait_ticks = 0;
                if (asp_ready) {
                    npc_wait_ticks = 0;
                    apply_enemy_action(THREAD_SCHED, 0);
                } else {
                    npc_wait_ticks++;
                    if (npc_wait_ticks >= g_cfg.npc_timeout_ticks) {
                        npc_wait_ticks = 0;
                        apply_enemy_action(THREAD_SCHED, 1);
                    }
                }
            }

            continue;
        }

        // WIN/GAMEOVER: wait for Enter (or 30s timeout) then exit
        if (phase == g_cfg.phase_win || phase == g_cfg.phase_gameover) {
            static int end_ms = 0;
            end_ms += 50;
            sleep_ms(50);
            if (key == (int)sf::Keyboard::Return) break;
            if (end_ms >= 30000) break; // 30-second safety timeout
            continue;
        }

        // PAUSE: freeze the scheduler until resumed
        if (g_paused) {
            if (key == (int)sf::Keyboard::Q) { g_running = 0; break; }
            sleep_ms(50);
            continue;
        }

        sleep_ms(20);
    }

    g_running = 0;
    if (window.isOpen()) {
        window.close();
    }
    rt.join();

    shm_destroy();
    return 0;
}
