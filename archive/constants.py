"""
Central place for all magic numbers and reverse-engineered values.

Everything here was derived from:
  - Reading COTM.exe's strings and disassembly
  - The FearlessRevolution community cheat table (v1.1.2 Steam)
  - Our own Frida-based experiments

If you change a value here, please add a comment explaining why.
"""

# ---------------------------------------------------------------------------
# Game
# ---------------------------------------------------------------------------
GAME_TITLE       = "Bloodstained: Curse of the Moon"
GAME_VERSION     = "1.1.2"
STEAM_APP_ID     = 838310
PLATFORM         = "steam-windows-x86"
TARGET_PROCESS   = "COTM.exe"

# ---------------------------------------------------------------------------
# Frida / hook sites (all module-relative offsets in COTM.exe)
# ---------------------------------------------------------------------------
# The input poll calls GetAsyncKeyState. The instruction immediately AFTER
# the call returns has this offset. We filter on it so we only intercept
# the game's own keyboard poll (ignoring D3D9, Steam overlay, drivers).
POLL_RET_OFFSET           = 0x2A5960

# VK table used by the game's poll loop. 81 DWORD entries.
VK_TABLE_OFFSET           = 0x3D1760

# The global at this offset holds a pointer the game loads right after the
# keyboard poll loop. It is NOT the input manager (it points to the process
# environment block). Kept here as a note of what we ruled out.
ENV_PTR_GLOBAL_OFFSET     = 0x490948

# ---------------------------------------------------------------------------
# Input state layout
# ---------------------------------------------------------------------------
# The game maintains two 81-byte arrays:
#   STATE_CUR[i]  = GetAsyncKeyState(VK_TABLE[i]) >> 15   (this frame)
#   STATE_PREV[i] = previous frame's STATE_CUR[i]
# STATE_CUR base is at "S"; STATE_PREV is at "S - 0x80".
STATE_ARRAY_LEN           = 81
STATE_CUR_PREV_DELTA      = 0x80

# ---------------------------------------------------------------------------
# Player struct (pointer chain from the community CT)
# ---------------------------------------------------------------------------
# Chain root: module_base + PLAYER_CHAIN_ROOT, then deref, then add the
# offsets in REVERSED order (deref each intermediate, last is a plain add).
PLAYER_CHAIN_ROOT         = 0x0048365C
PLAYER_CHAIN_OFFSETS      = [0x3DC, 0x84, 0x08, 0x20, 0x20, 0x6C, 0x08]

# Player struct fields (offsets from the resolved struct base = "p6")
PLAYER_OFF_HEALTH         = 0x3DC   # u8
PLAYER_OFF_INVISIBILITY   = 0x53C   # f32
PLAYER_OFF_XVEL           = 0x1A0   # f32
PLAYER_OFF_YVEL           = 0x1A4   # f32
PLAYER_OFF_X              = 0x1AC   # f32  (physics)
PLAYER_OFF_Y              = 0x1B0   # f32  (physics)
PLAYER_OFF_X_RENDER       = 0x5B8   # f32  (render copy, +-8 snap)
PLAYER_OFF_Y_RENDER       = 0x5BC   # f32

# ---------------------------------------------------------------------------
# Secondary root used by simpler chains (difficulty, ammo, characters, ...)
# ---------------------------------------------------------------------------
SIMPLE_CHAIN_ROOT         = 0x00483660
SIMPLE_CHAIN_COMMON       = 0x08     # first offset after deref

# ---------------------------------------------------------------------------
# Tracked input keys.
#
# name -> (vk_code, vk_decimal_as_string)
#
# We record and replay at the VK layer because the game has no separate
# action-flag variable (verified experimentally: the state array is read
# directly by the game logic each frame).
# ---------------------------------------------------------------------------
TRACKED_KEYS = {
    # movement
    "LEFT":   (0x25, "37"),
    "RIGHT":  (0x27, "39"),
    "UP":     (0x26, "38"),
    "DOWN":   (0x28, "40"),
    "A":      (0x41, "65"),
    "D":      (0x44, "68"),
    "W":      (0x57, "87"),
    "S":      (0x53, "83"),
    # combat
    "SPACE":  (0x20, "32"),
    "LMB":    (0x01, "1"),
    "RMB":    (0x02, "2"),
    # character switching
    "Q":      (0x51, "81"),
    "E":      (0x45, "69"),
    # pause / menu
    "P":      (0x50, "80"),
    "ENTER":  (0x0D, "13"),
    "ESC":    (0x1B, "27"),
}

# Reverse map (VK decimal string -> friendly name)
VK_DEC_TO_NAME = {dec: name for name, (vk, dec) in TRACKED_KEYS.items()}

# ---------------------------------------------------------------------------
# Movie file format
# ---------------------------------------------------------------------------
MOVIE_FORMAT          = "bscotm-tas"
MOVIE_FORMAT_VERSION  = 1

# ---------------------------------------------------------------------------
# Replay tuning
# ---------------------------------------------------------------------------
# Target FPS during "silent replay". The real game runs at 60; we push
# higher during rewind so the tool stays usable.
REPLAY_TARGET_FPS     = 500
REPLAY_SETTLE_FRAMES  = 5      # frames to run after reaching target before pause