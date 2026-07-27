# Plan: GamePad Mapper + GamePad Support Library

Two products on one foundation. `padkeys` proved the mechanism (see `README.md`);
this is how it becomes something users install and developers build against.

## The shared foundation: `gamepadd`

Rather than two separate programs, evolve `padkeys` into a single root daemon with
two front-ends. It already does the hard part — enumerate pads, read evdev, inject
via uinput.

```
   USB/BT pad ──► gamepadd ──┬──► uinput virtual keyboard   (unmodified games)
                             └──► /tmp/gamepadd.sock         (library-aware games)
```

Changes needed to `padkeys` to become `gamepadd`:

1. **Hotplug** — currently scans `/dev/input` once at startup. Add an inotify watch
   so pads can be plugged and unplugged freely (same trick hidd uses).
2. **Config file** — read mappings from JSON (`/media/internal/.gamepad/config.json`)
   instead of the compiled-in tables, with named profiles.
3. **Socket server** — a unix socket publishing normalized pad state, mode `0666`
   so unprivileged games can connect (see the permission note below).
4. **Control channel** — enable/disable keyboard emulation, switch profile, and a
   "learn" mode that streams raw button presses so the Mapper UI can bind them.
5. **Stay-awake** — hold a powerd activity while a pad is active, so the screen
   doesn't sleep during controller-only play. (webOS autosleep was a recurring
   nuisance in testing; a gamepad generates no touch events.)

Two constraints that shape everything:

- **`/dev/input/event*` is `root:root 0640`** — PDK games run unprivileged and
  *cannot* read evdev directly. The daemon must run as root and serve them. (A udev
  rule relaxing permissions is the alternative, but it's a system-wide change and
  the socket is cleaner.)
- **SDL 1.2's joystick API is a dead end** — `CONFIG_INPUT_JOYDEV` is unset, so
  there is no `/dev/input/js*` for it to open. Games cannot use the obvious path;
  hence the library.

## 1. GamePad Mapper (companion app)

An Enyo app the user launches to turn gamepad support on and configure it.

**Screens**
- *Status* — detected pads (name, type, battery if known), big enable/disable toggle
- *Mapping* — list of controls; tap a row, press the button on the pad, it binds
  (uses the daemon's learn mode); analog stick sensitivity/deadzone sliders
- *Profiles* — save named mappings; suggested presets for common games

**The awkward part: launching a root daemon from an app.** webOS apps and JS
services run unprivileged, and this unit has no `org.webosinternals.ipkgservice`
installed. Options in order of preference:

1. **Package `gamepadd` with a postinst that installs it setuid-root** and have a
   Node.js JS service `child_process.exec` it. Self-contained, no Preware
   dependency. Needs care: a setuid binary that opens uinput must not take
   arbitrary paths from its command line.
2. Depend on `ipkgservice` (Preware) to run it as root — simplest to build, but
   adds an install dependency and it isn't present by default.
3. postinst-installed upstart job that the app starts/stops via its JS service —
   rejected for now, since the point is that the user opts in by launching the app.

**Distribution**: `.ipk` for App Museum II / Preware. App ID should start with
`com.palm.` if it ends up needing any privileged Luna calls.

## 2. GamePad Support Library (PDK)

A small static C library games link against for native support — no Mapper, no
key-mapping fiction, real axes and buttons.

**API sketch**

```c
int  wospad_init(void);                  /* connects to gamepadd socket */
int  wospad_count(void);                 /* pads currently connected    */
int  wospad_poll(int pad, wospad_state *out);
void wospad_shutdown(void);

typedef struct {
    uint32_t buttons;      /* bitmask, WOSPAD_BTN_* — normalized across pads */
    float    lx, ly, rx, ry;   /* -1.0 .. 1.0, deadzone already applied      */
    float    lt, rt;           /*  0.0 .. 1.0                                */
    int8_t   hatx, haty;       /* -1, 0, 1                                   */
} wospad_state;
```

**Design points**
- **Normalize across controllers** so a game written for a DS4 works with the
  Logitech pad: same bitmask, absent controls simply read zero.
- **Ship as `libwospad.a` + one header**, buildable in the standard PDK toolchain;
  no dependency on the daemon's internals, just the socket protocol.
- **Graceful degradation** — if `gamepadd` isn't running, `wospad_init()` fails
  cleanly and the game falls back to touch controls. Never a hard requirement.
- **Optional SDL 1.2 glue** — a helper that translates pad state into SDL key or
  user events for retrofitting existing games with minimal surgery.
- **Document the socket protocol** so ports in other languages/engines are possible.

**Retrofit path for existing games**: most PDK games already read SDL keyboard
events, so they work through the Mapper today with zero changes. The library is
for new development and for retrofits that want analog control, which key mapping
fundamentally cannot provide.

## Sequencing

1. `gamepadd` (hotplug + config file + socket) — everything else depends on it
2. Library + a sample game, since it validates the socket protocol
3. Mapper app — most work, and the design benefits from the daemon being settled

Bluetooth support drops into this without redesign: if PmBtEngine's HID reports
can ever be liberated (see `README.md`), they become another input source inside
`gamepadd` and both products gain BT pads for free.
