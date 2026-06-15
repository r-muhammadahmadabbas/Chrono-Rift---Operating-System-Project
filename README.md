<div align="center">

# ⚔️ Chrono Rift

### Multi-Process Tactical Combat Game

*A fully playable game where every mechanic maps directly to an Operating Systems concept*

![C++](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square&logo=cplusplus&logoColor=white)
![Linux](https://img.shields.io/badge/Platform-Linux-FCC624?style=flat-square&logo=linux&logoColor=black)
![SFML](https://img.shields.io/badge/Graphics-SFML-8CC445?style=flat-square)
![Docker](https://img.shields.io/badge/Docker-Ready-2496ED?style=flat-square&logo=docker&logoColor=white)
![Course](https://img.shields.io/badge/CS2006-Operating%20Systems-red?style=flat-square)

**FAST-NUCES Islamabad · CS 2006 Operating Systems · Spring 2026**

</div>

---

## 📖 Overview

Chrono Rift is a multi-process tactical turn-based combat game built entirely in **C++** using **Linux system calls** for inter-process communication and **SFML** for graphical rendering. Every game mechanic is a direct implementation of a core Operating Systems concept — from process isolation and shared memory IPC to signal-driven interrupts and deadlock detection.

The game pits a party of **1–4 human-controlled heroes** against **2–9 computer-controlled enemies**. A stamina-based temporal scheduler determines turn order, and all communication between processes occurs through **System V Shared Memory** protected by **Lamport's Bakery Algorithm**.

> **No game engine. No shortcuts. Every mechanic is a real OS concept.**

---

## 🧠 OS Concepts → Game Mechanics

| OS Concept | Implementation | Game Mechanic |
|---|---|---|
| **Process Isolation** | 3 independent binaries | Arbiter, HIP, ASP each compiled separately |
| **IPC (Shared Memory)** | `shmget` / `shmat` | All state shared — zero pipes anywhere |
| **Mutual Exclusion** | Lamport's Bakery Algorithm (1974) | Guards every read/write to shared memory |
| **Multithreading** | `pthread_create` | One thread per hero (HIP), one per enemy (ASP) |
| **CPU Scheduling** | Stamina-based temporal scheduler | Arrival time formula determines turn order |
| **Signals (Async)** | `sigaction`, `kill`, `alarm` | Stun, Ultimate Ability, graceful quit |
| **Memory Management** | First-Fit Contiguous Allocation | 20-slot weapon inventory with auto-eviction |
| **Deadlock Detection** | Circular-wait detection loop | Artifact contention resolved every scheduler tick |

---

## 🏗️ Architecture

Three completely independent processes, each compiled into its own binary:

```
┌─────────────────────────────────────────────────────────────────┐
│                     SYSTEM V SHARED MEMORY                       │
│                                                                  │
│   GameState { entities[], hip_buf, asp_buf, BakeryLock(main),   │
│               BakeryLock(res), keyboard_buf, action_log,         │
│               resource_table, drop_state }                       │
│                                                                  │
└──────────────┬────────────────────┬──────────────────┬──────────┘
               │  shmget / shmat    │                  │
       ┌───────▼────────┐   ┌───────▼───────┐   ┌──────▼──────┐
       │    ARBITER     │   │      HIP      │   │     ASP     │
       │  arbiter_bin   │   │   hip_bin     │   │   asp_bin   │
       │                │   │               │   │             │
       │ • Owns memory  │   │ • Keyboard    │   │ • NPC AI    │
       │ • Scheduler    │   │ • 1 pthread   │   │ • 1 pthread │
       │ • SFML render  │   │   per hero    │   │   per enemy │
       │ • Deadlock chk │   │ • Writes only │   │ • Writes    │
       │ • Sends signals│   │   to hip_buf  │   │   to asp_buf│
       └────────────────┘   └───────────────┘   └─────────────┘
```

**Design Rule:** Only the Arbiter writes game state fields. HIP writes only to `hip_buf`. ASP writes only to `asp_buf`. Write conflicts are eliminated by construction.

---

## 🔒 Synchronization: Lamport's Bakery Algorithm

Synchronization uses **Lamport's Bakery Algorithm** configured for 3 participants, implemented using only plain `int` variables in shared memory — no OS semaphores or mutexes.

```c
typedef struct {
    int choosing[3];
    int number[3];
} BakeryLock3;
```

**Dual-lock architecture** prevents unnecessary contention:
- `lock` — protects main game state (entities, buffers, phase)
- `res_lock` — protects the artifact resource table (Solar Core, Lunar Blade, Eclipse Relic)

Compiler memory barriers (`__asm__ ("" ::: "memory")`) prevent the optimizer from reordering memory operations across lock boundaries.

---

## 📡 Signals & Asynchronous Interrupts

| Signal | Trigger | Effect |
|--------|---------|--------|
| `SIGUSR1` | High-tier attack | Stuns target process for 3 seconds (`sleep(3)`) |
| `SIGSTOP` | Ultimate Ability activated | Freezes ASP — all enemy threads paused |
| `SIGALRM` | `alarm(10)` fires | Handler sends `SIGCONT` to resume ASP |
| `SIGTERM` | Player presses `Q` | HIP → Arbiter; triggers shared memory cleanup |

No flag polling. No pipes. Pure POSIX signal semantics.

---

## ⏱️ Temporal Scheduler

The Arbiter runs a stamina-based scheduler with a **100ms tick**:

$$T_{arrival} = \frac{S_{max} - S_{current}}{speed}$$

| Entity | Speed | Max Stamina | Turn Frequency |
|--------|-------|-------------|----------------|
| Player (4 heroes) | 25 | 100 | Every ~400ms |
| Enemy | ~20 | 150 | Every ~750ms |

The entity with the smallest arrival time acts first. Execution is strictly serial — one entity at a time — ensuring predictable, deterministic turnaround with zero scheduling conflicts.

---

## 🎒 Weapons & Inventory System

Each player has a **20-slot linear inventory** managed by a **First-Fit Contiguous Allocator**:

1. `inventory_find_first_fit()` — scans for the first free contiguous region
2. If no space: `inventory_list_runs()` identifies all weapon runs
3. `inventory_evict_run()` removes the minimum weapons to long-term storage
4. New weapon is placed in the freed space

| Weapon | Slots Required | Damage |
|--------|---------------|--------|
| Solar Core | 10 | 95 |
| Lunar Blade | 10 | 90 |
| Iron Halberd | 7 | 55 |
| Thunderstaff | 6 | 50 |
| Frostbow | 6 | 48 |
| Obsidian Axe | 5 | 45 |
| Venom Dagger | 4 | 30 |
| Splinter Stick | 2 | 12 |

> **Hold both the Solar Core and Lunar Blade** to unlock the **Ultimate Ability** — which fires `SIGSTOP` to freeze all enemies for 10 seconds.

---

## ☠️ Deadlock Detection & Resolution

A shared `ResourceTable` tracks ownership and waiters for three global artifacts. The Arbiter calls `deadlock_check_and_resolve_unsafe()` every scheduler tick.

**Detection:** Circular wait — entity A holds Solar Core and waits for Lunar Blade; entity B holds Lunar Blade and waits for Solar Core.

**Resolution:** The Arbiter forces the Lunar Blade holder to release it, breaking the cycle. The **Eclipse Relic** joins the artifact pool dynamically — 20% chance of appearing on any weapon drop.

---

## 🎨 Graphics & Rendering

Built with **SFML** in a dedicated `std::thread` that reads a snapshot of shared memory under the Bakery lock once per frame at **60 FPS**. The scheduler is never blocked by rendering.

**Visual features:**
- Real-time HP and stamina bars for all entities
- Animated sprites (6-frame sheets: 3 idle + 3 attack, 32×32 px)
- Per-hero textures (`Hero_1.png` through `Hero_4.png`)
- Weapon arsenal grid with active-weapon highlight
- Scrolling action log
- Floating damage numbers (red for player hits, yellow for enemy hits)
- Victory / Game Over overlays with match statistics
- Pause overlay (`ESC`) and controls panel (`F1`)

---

## 🚀 Build & Run

### Prerequisites

**Option A — Docker (Recommended)**
```bash
docker build -t chrono-rift .
docker run -it --rm \
  -e DISPLAY=$DISPLAY \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  chrono-rift
```

**Option B — Native Linux (Ubuntu 22.04+)**
```bash
sudo apt-get install libsfml-dev
```

### Build

```bash
make
```
Produces three binaries: `arbiter_bin`, `hip_bin`, `asp_bin`.

### Single Player

```bash
./asp_bin &
./hip_bin 0 1 2 3 &      # Controls heroes 0–3
./arbiter_bin
```

### Local Multiplayer

```bash
./asp_bin &
./hip_bin 0 1 &           # Player 1 controls heroes 0 and 1
./hip_bin 2 3 &           # Player 2 controls heroes 2 and 3
./arbiter_bin
```

---

## 🎮 Controls

| Key | Action |
|-----|--------|
| `S` | Strike (attack selected enemy) |
| `K` | Skip (pass turn; recover 50% stamina) |
| `H` | Heal |
| `W` | Swap weapon to/from long-term storage (costs a full turn) |
| `U` | Ultimate Ability (requires Solar Core + Lunar Blade) |
| `ESC` | Pause / Resume |
| `F1` | Toggle controls help panel |
| `Q` | Quit game gracefully |

---

## 🏆 Win & Loss Conditions

| Condition | Result |
|-----------|--------|
| Kill 10 enemies | **Victory** — match statistics displayed |
| All heroes defeated | **Game Over** |
| Multiplayer — equal kills | **Draw** |
| Multiplayer — both teams wiped | **Both Teams Defeated** |
| Press `Q` | Graceful exit via `SIGTERM`; shared memory cleaned up |

---

## 🔢 Roll Number Seed

All randomised elements use seed **243058** (derived from roll number I24-3058):

| Parameter | Value |
|-----------|-------|
| Player HP | `243058 + random(100, 1000)` |
| Enemy HP | `58 + random(50, 200)` |
| Player Damage | `8 + 10 = 18` |
| Enemy Damage | `5 + 10 = 15` |
| Player Speed | `100 / (number of players)` |
| Enemy Speed | `random(10, 30)` |

---

## 👥 Team

| Name | Roll Number |
|------|-------------|
| Muhammad Ahmad Abbas | I24-3058 |
| Hassan Amir | I24-0556 |

**Submitted To:** Dr. Faisal Cheema  
**Course:** CS 2006 Operating Systems · Semester 4 · Spring 2026  
**University:** FAST-NUCES, Islamabad Campus

---

## 📚 References

1. A. S. Tanenbaum and H. Bos, *Modern Operating Systems*, Pearson, 4th Edition, 2015.
2. L. Lamport, "A New Solution of Dijkstra's Concurrent Programming Problem," *Communications of the ACM*, vol. 17, no. 8, pp. 453–455, 1974.
3. SFML Development Team, [SFML Documentation](https://www.sfml-dev.org/documentation/), 2024.

---

<div align="center">

Made with ⚙️ and too many race conditions &nbsp;·&nbsp; FAST-NUCES Islamabad &nbsp;·&nbsp; 2026

</div>
