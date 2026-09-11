# Battery usage

The screen Android has and Plasma Mobile does not: the apps ordered by what they
spend of the battery. The cell's state is not reimplemented — the header links to
Plasma's Energy page, which already exists.

**It works, it is packaged and in the catalogue.**
What is missing —measuring the residue of a real day and calibrating so the screen
and system rows appear— is in
[`tasks/018`](../../../tasks/018-battery-consumption-per-app.md). Here goes
only how it works.

---

## The two hard parts

Neither is the interface. They are these.

### 1 · Where "how much each app spent" comes from

There is no sensor that says it. On an ARM phone **there is no RAPL**, there are no
per-process energy counters, and the battery gauge only knows the total. Android
solves it with a `power_profile.xml` that the manufacturer measures on a test bench,
plus a kernel patch (`uid_time_in_state`) that **never went upstream and that Google
deleted from its own branch**.

What there is on this phone, and it is more than it seems:

| | |
|---|---|
| **CPU per app** | systemd already puts each app in its cgroup: `app.slice/app-*.service/cpu.stat` |
| **Power curve** | `/sys/kernel/debug/energy_model/`, in real microwatts per frequency |
| **GPU per process** | `drm-cycles-gpu` in `/proc/<pid>/fdinfo/` |
| **Real total power** | `qcom_qg`: `current_now` × `voltage_now` |

With that, the split is done **without believing the watts**:

1. The interval's real energy is measured — `I × V × Δt`. That is not a model.
2. A weight per app is computed from its CPU and GPU time.
3. The real energy is split in proportion to the weights.

The percentages thus add up to 100 % of what really came out of the battery, and the
model's scale errors cancel out in the division.

### 2 · What the energy model is for, which is not what it seems

This was discovered writing `collector.cpp` and it is worth not tripping on again:

> **The energy model cannot separate two apps from each other.** The same
> coefficient multiplies both, so it **cancels out** in the proportional split.
> Decorating the weight with it changes not a single percentage.

What it does do is say **how much of the measured energy was even CPU**:

```
E_cpu_modelled = (CPU time of the WHOLE machine) x (mean mW of a busy core)
```

and that number is the **ceiling** of what the apps can split among themselves.
Without it, the consumption of the modem and the radios on an idle phone would end
up billed to the first app that happened to be awake. Everything above the ceiling
goes to the **"system"** bucket, which is shown as-is instead of hiding it: if it
comes out huge, the model is wrong and you see it at a glance.

---

## What this split does wrong, and it is known

`cpu.stat` gives microseconds, **not which core they were spent on**. On this SoC an
A76 at 2.3 GHz costs 968.8 mW and an A55 at 300 MHz costs 13.8 mW: **a factor of
70**. Weighting the microseconds of all apps by the same figure is exactly the error
that makes Scaphandre useless on an asymmetric SoC.

It is not an oversight: it is phase 1 on purpose. What is sought from it is **the
residue** — how far the modelled total drifts from the gauge over a real day. That
number decides whether phase 2 is needed, which is an eBPF program over
`sched_switch` + `cpu_frequency` and forces recompiling the kernel with
`CONFIG_DEBUG_INFO_BTF`.

---

## The files

| | |
|---|---|
| `calibrate` | Phase 0: measures the real power per frequency point and per brightness. **Done.** The real run is missing |
| `try-calibrate` | Exercises the whole `calibrate` against a fake sysfs, with no hardware. Passes **on the surya itself** |
| `CMakeLists.txt` | Builds native on the phone, which is where the Qt6 is |
| `app-usaged.service` | The user service |
| `app-usage.desktop`, `app-usage.svg` | «Battery usage» in the launcher, and its colour icon |
| `main.qml` | The window, with the link to the energy panel |
| `AppsPage.qml` | The per-app list |
| `Ink.qml`, `Format.qml` | Common palette and formatting |
| `i18n/app-usage_es.ts` | Spanish translation |
| `src/collector.{h,cpp}` | The sampling and the split |
| `src/store.{h,cpp}` | Hourly aggregates in `~/.local/state/app-usage`, one file per day |
| `src/appinfo.{h,cpp}` | From the cgroup id to the name, the icon and **whether it is in the drawer** |
| `src/launcher.{h,cpp}` | Opens Plasma's Energy panel |
| `src/usage.{h,cpp}` | The model the screen consumes |
| `src/main.cpp` | The daemon and the report |
| `src/ui.cpp` | The screen |
| `src/try-collector.cpp` | Two sweeps and the split, to see what it really reads |
| `src/try-appinfo.cpp` | That the drawer/background separation is right, against real units |

Three names that are **not** free:

- **`slots` cannot be used as a variable name.** It is a Qt macro that expands to
  nothing, so `QVector<QDateTime> starts(slots)` becomes `starts()`, a function
  declaration, and the compiler complains about everything but the cause.
- The palette singleton is called `Ink`, **not `Palette`**. QtQuick has its own
  `Palette` since 6.6, so `import QtQuick` brings it into scope and wins. The symptom
  is not a name clash: it is a whole screen of «Unable to assign [undefined]» because
  each property was read from the wrong type.
- The window is `app-usage`; the daemon is `app-usaged`, with the usual `d`. Pointing
  the launcher at the daemon would put a second collector sampling in the foreground,
  writing the same files as the service.
- The visible name **is not translated**. «Battery usage» is what the program is
  called, just as Spectacle is Spectacle in every language; a launcher entry that
  changes name by language is one you cannot tell anyone to look for.

### How it is used

```sh
systemctl --user enable --now app-usaged  # the daemon
app-usage                                 # the screen
app-usaged --informe --periodo 24h        # the same list, without a compositor
```

⚠️ **`--forzar` requires `--state <dir>`, on purpose.** It forces intervals measured
*with* the charger, where the battery barely flows and the joules mean nothing.
Writing them to the real store leaves it contaminated without it showing —the numbers
come out, and they are a lie—, which is exactly what happened here: 42.8 fake J that
had to be found and deleted by hand. Having the flag name its own directory makes it
impossible instead of merely discouraged:

```sh
app-usaged --demonio --forzar --state /tmp/test
app-usaged --informe --state /tmp/test
```

They are **three binaries from a single library**, and the separation matters: the
daemon is only QtCore and runs for days, so linking QtQuick into it would put the
whole graphics stack in a process that draws nothing — and would make the biggest
resident precisely the thing that has to be cheap.

### Viewing the screen without a compositor

`app-usage --captura file.png` draws one frame, saves it and exits. With
`QT_QPA_PLATFORM=offscreen` it needs no compositor, no unlocked session, and no panel
on:

```sh
QT_QPA_PLATFORM=offscreen app-usage --captura /tmp/ui.png
```

### The residue, which is phase 1's deliverable

The store keeps a `modelled` row with what the energy model says the apps cost,
alongside the gauge's `measured`. The sentence begins with **how many hours of
measurement** back it, and that is not decoration: a percentage computed over twenty
minutes is noise with a percent sign, and from the number alone it cannot be told
apart from a good one.

```
1 h measured: model 0.22 J for 2.3 s of app CPU, against 6.08 J real (-96 %)
```
 The difference between
the two **is the number that decides whether phase 2 is needed** with eBPF over
`sched_switch`, and it comes out always —in the report and at the foot of the
screen—, not only when it looks bad: hiding it under a threshold would be hiding the
decision.

⚠️ Until 2026-08-28 that number **was computed on each interval and thrown away**. The
comment in `IntervalResult` said that comparing it with what was measured over a real
day was phase 1's criterion, and it was not saved anywhere: it was incomputable.

### Calibrating

Three guards that exist because the three faults happened for real:

```sh
app-usage-calibrate --solo brillo              # 4 min, unplugged
app-usage-calibrate --solo brillo --continuar  # resumes if it was cut off
```

**1 · Checks that consumption is steady BEFORE spending the run.** All the points are
deltas over the baseline, so if consumption moves on its own while the baseline is
measured, *all* of them come out wrong. The control's drift already caught it —but at
the end—. On 2026-08-29, launched three minutes after a boot, the control drifted
**−261.8 mW** and two minutes had to be thrown away. Now it is two 8 s samples and it
aborts if they differ by more than 25 mW.

**2 · `--continuar` resumes.** An 18-minute run never finishes on a phone that reboots
every two. The CSV was already written point by point; all that was missing was
reading it on start. **It does not reuse the baseline** even if it is in the file: it
is from another session, at another temperature, and reusing it would be mixing two
experiments.

**3 · The log is written line by line, not in blocks.** Redirected to a file —which is
how it runs— Python buffers 8 KB and only flushes them on finishing. When the phone
powered off midway, the log came out **empty** and there was no way to know how far it
had got.

And a warning that is not about the measurement but about the current: if `--con-cable`
accepts, the input is pinned — which means **also** that the phone is already drawing
from the cell, and this run adds load on top. See
[BOOT-03](../../../docs/BOOT-03-current-deficit.md).


⚠️ **Until 2026-08-28 the calibration did not reach the daemon.** `calibrate` wrote a
CSV and the daemon read an INI, and **nothing joined the two**: you could measure
everything and the «Screen» and «System» rows stayed empty forever. The CSV is the
raw data, useful for looking point by point; the file that rules is
`~/.config/app-usage/calibration.ini`, and now `calibrate` writes it.

**The calibration is two halves and they are distinguished.** `Calibration` has
`cpuKnown` and `screenKnown`, not a single `loaded`, because they are measured
separately:

| | lasts | what it enables |
|---|---|---|
| `--solo brillo` | ~4 min | the «Screen» row |
| the full run | ~18 min | additionally the split's **ceiling**, and with it «System» |

Treating a brightness pass as "calibrated" would turn on the ceiling with the
**unscaled** milliwatts from the device tree, which measures the calibration that is
missing instead of the model that is missing. That is why presence is detected by the
**key** in the INI, not by a plausible value: an absent `[cpu]` means "not measured"
and has to be told apart from "measured and came out 1.0".

The brightness half is in the app, in the button at the foot. It only appears
unplugged, because plugged in `calibrate` refuses and it would be four minutes for
nothing. The bar is moved by the clock and not by analysing `calibrate`'s output: its
progress lines are prose for a person, and turning them into a machine interface
would freeze that prose as an API.

And the process exiting with 0 is not enough: `calibrate` **refuses to write** if the
control baseline drifted more than 20 mW, and exits with 0 having said so. Success is
checked by looking at whether the file already has a screen section.

### The icon

It is in colour on purpose, and **drawn to survive 22 px and both backgrounds**. The
first version had the dark-grey shell: in the phone's dark drawer it left three bars
of colour floating in a smudge — colourful in the file and illegible where it is
really looked at. Now the body is a light fill with a dark outline: the fill holds it
up on a dark background, the outline on a light one, and neither depends on the theme.

⚠️ **`--` is not valid inside an XML comment.** The SVG stopped loading with
`Expected '>', but got ' '` over a double dash in the comment's prose, which points
to the cause not at all.

⚠️ **Plasma does not re-read the icon directories.** The file can be perfect and
`kiconfinder6 app-usage` find it, and still a generic icon comes out in the drawer:
`KIconLoader` keeps the list it read at startup. That is why the setting script sends
the `org.kde.KIconLoader.iconChanged` signal, just as `setup/lib/remoto` does with the
flatpaks.

It exists because checking that the screen draws ended in a fight with the compositor:
`spectacle` is single-instance and D-Bus activated, so a hung `spectacle --dbus` eats
all the following requests **and reports success without writing anything**; and when
it did fire, the panel was off and it captured a white rectangle. None of that says
whether the QML is right.

The report is the same pipeline that will draw the QML screen, so whatever comes out
wrong in the numbers comes out wrong here too — and it is read over ssh.

### Why it does not measure with the cable in, not even while discharging

It can happen —and it does— that the gauge says **`Discharging` with the charger
connected**: the phone asks for more than the USB gives and the cell covers the
difference. Measured here at 28 % battery: the input pinned at 2395 mW ±5, and one A76
thread moving the battery 659 mW and the input 0.9 mW.

Even so **it is not recorded**, and the reason is not that there is nothing to measure
but that what comes out of the cell is *only the part the USB does not cover*, while
the model estimates the **whole** consumption of the apps. The residue would come out
biased by construction — and the residue is phase 1's deliverable.

(For `calibrate` it does work, and that is why it has `--con-cable`: there everything is
*deltas* over a baseline, and a constant contribution cancels out in each subtraction.)

### What it takes up

Measured on real data from this phone: one hour with 19 active apps takes up **977
bytes**. Extrapolated to the full 8-day retention that is about **183 KB**. There is no
need to compress anything or trim the retention.

### What it costs

Measured on the service's own `cpu.stat`, with 30 s sweeps:

| | per sweep | of one core |
|---|---|---|
| first version (DRM rescan every 60 s) | 50.8 ms | 0.17 % |
| now (rescan on changes) | **30.1 ms** | **0.10 %** |

It is still above the 5.4 ms that the reconnaissance estimated for the plain cgroup
sweep, and the difference is the ~60 reads of `/proc` needed for the GPU (the `cgroup`
and the `fdinfo` of each of the ~30 DRM descriptors) plus the amortised full rescan.
In absolute terms it is ~0.5 mW against an idle of 300-400 mW, so it is not chased
further for now — but it is measured, not assumed, and it is the place where it would
grow if someone touches the sampling.

### Packaging

Follows the repository's pattern, just like `screenglaze`:

| | |
|---|---|
| `setup/ajustes/app-usage.d/` | the code, **the copy that rules** |
| `setup/ajustes/app-usage` | the setting script |
| `pmaports/temp/app-usage/APKBUILD` | the recipe |
| `pmaports/sincronizar app-usage` | copies `.d/` → `temp/` and recomputes sums |
| `setup/apps.conf` | the catalogue line, with its check |

```sh
surya/pmaports/sincronizar app-usage
pmbootstrap build app-usage --arch aarch64
APP_USAGE_APK=/path/a/app-usage-1.0-r0.apk surya-setup app-usage
```

⚠️ **If you add a file it goes in THREE places**: the `sincronizar_app_usage()` list,
the APKBUILD's `source=` and the CMakeLists' `QML_FILES` (or the `add_executable`).

Two things about the `APKBUILD` that are not decoration:

- **The `prepare()` undoes the symlinks.** `abuild` does not copy the sources to
  `$srcdir`, it *links* them, and CMake's `install(FILES)` preserves the link. The
  package would come out with `app-usage.desktop` pointing to `/home/pmos/build/`,
  which does not exist on the phone: `apk info -L` lists it, `ls` shows it and **the
  app does not appear in the launcher**. `poconav` and `screenglaze` already stepped
  on that trap.
- **It is built with `-DAPP_USAGE_DIAGNOSTICOS=OFF`.** The harnesses are not
  installed, so compiling them inside `abuild` would only cost time, and their sources
  do not even go in `source=`.

`calibrate` is packaged, as `/usr/bin/app-usage-calibrate`: without it an installed
system could never produce the calibration the daemon reads, and the «Screen» and
«System» rows would be impossible forever.

### Drawer apps versus background services

The rule is **the launcher's own**, not an approximation: there is a `.desktop` in the
application folders and no `NoDisplay`, no `Hidden`, and no `OnlyShowIn`/`NotShowIn`
that excludes KDE. Being literally the XDG criterion, it cannot disagree with what the
drawer shows.

Measured over the surya's 32 live units, and saved as a test in `try-appinfo`:
**Spectacle is the only one launched from the drawer**. The other two the rule accepts
—Screenglaze and Battery— do have an entry even though what spends their CPU is a
daemon. Left out, correctly, are the 3 with `NoDisplay=true`, the 9 with an
`@autostart` suffix and the 9 D-Bus activated with an `app-dbus-:1.1-` prefix.

**They are shown by default**, only marked. On this phone the biggest consumer is the
shell itself, which no one "opened": a meter that by default hides that is hiding the
answer. The "Only apps" filter is one tap away.

### The battery state is not reimplemented: it is linked

This app shows **one single thing**: where the battery went, per app. For the cell's
state —charge, health, technology, charge graph— the phone **already has a screen**,
Plasma's Energy one, and it is better than any copy because it is the one that updates
with Plasma. The header carries a button that opens it.

Embedding it was discarded: its `main.qml` calls `kcm.push()` on a C++ object
(`KQuickConfigModule`) that the host would have to create, and its QML module is called
`org.kde.kcm.power.mobile.private` — private and unversioned, meaning any update could
take out half the app.

⚠️ **`plasma-settings -m` does not work to open it, even though `-l` lists it.**
Measured on this phone, with no settings window open:

| command | result |
|---|---|
| `kcmshell6 kcm_mobile_power` | window `[kcm_mobile_power]` titled **«Energy»**, no errors |
| `plasma-settings -m kcm_mobile_power` | `unable to find module`, and a «Settings» window — the **list** |
| `plasma-settings -m kcm_mobile_power -s` | same |

That is why `launcher.cpp` tries **`kcmshell6` first**, with `plasma-settings` as a
fallback: leaving someone in the settings list is worse than the right page, but better
than a button that does nothing. Nor is the KCM's own `.desktop` used, which here says
`Exec=systemsettings` and **systemsettings is not installed**.

In passing, what that panel shows and this phone cannot give: its **Health** row is
`battery.capacity`, and `qcom_qg` reports `charge_full = 0`, so UPower falls back to
the design capacity and out comes a **100 % that means "I don't know"**. The cycles are
−1. Worth knowing before believing that figure.

### i18n

Source strings in English and the translation in `i18n/app-usage_es.ts`, compiled to
`.qm` and **embedded in the binary** so program and translation cannot go out of sync.
`lrelease` is called directly instead of using `qt_add_translations()`, because the
phone has `qt6-qttools` (the programs) but not its CMake package — and needing a module
that is not installed would mean not being able to build on the only machine that can
build.

On touching strings:

```sh
lupdate6 -source-language en -target-language es_ES src/*.cpp src/*.h *.qml -ts i18n/app-usage_es.ts
```

**Calibrating is not needed for the list.** Each app's weight is
`cpuMwMean × (dCpu + dGpu/1000)`, and `cpuMwMean` is the same number for all, so it
cancels out exactly in the proportional split: the order and the percentages *between
apps* depend neither on the calibration nor on the kernel's energy model. What does
depend is the **size of the bag** — how much goes to apps, how much to screen and how
much to «system». Without calibrating there is no screen row and «system» comes out at
zero.

### How it is built

On the phone itself. It ships `cmake`, `g++` and `qt6-qtbase-dev`, and it is aarch64,
i.e. the target architecture and not a cross approximation:

```sh
ssh surya
cmake -S . -B build && cmake --build build -j4
./build/try-collector 10            # two sweeps 10 s apart
./build/try-collector 10 --forzar   # in addition, the split with the cable in
```

⚠️ **Never read kernfs with `QTextStream`.** sysfs, procfs and cgroupfs do not give a
usable `st_size` —`cpu.stat` says 0 and has 156 bytes; `time_in_state` says 4096 and
has 134— and `QTextStream` believes it. On the first it reads nothing and all the apps
come out at zero; on the second it never reaches the end and pins a core at 100 %. Both
faults are silent. Use `QIODevice::readAll()`, which is the only one that reads
incrementally when the size is unknown: that is what `collector.cpp`'s `readFile()` is
for.

### `calibrate`

It is run **with the phone unplugged**, and it refuses to start on its own if it
detects a charger. It is not zeal: with the cable in the battery does not flow
—measured, −0.37 mA on average— and in one attempt to calibrate plugged in the baseline
drifted 66 mW with a deviation of 119. The data are worth nothing.

```sh
./calibrate --output ~/calibration.csv       # ~18 min
./calibrate --solo cpu --window 10          # a quick pass
```

Three decisions that are not obvious:

- **Orthogonal synthetic loads, not real use.** With real use the CPU frequency and the
  GPU rise together and the regression cannot separate them. Pinning one frequency in a
  cluster with the `userspace` governor and burning *one* thread with `taskset`, each
  measurement isolates one variable and the fit becomes arithmetic:
  `P_opp = P_measured − P_idle`. NNLS is not needed.
- **Everything with the backlight at zero.** Each point is a delta over the baseline, so
  the baseline has to hold steady for the 18 minutes. Measured with whatever brightness
  there was, it does not hold: the screen dims on its own —no one is touching it— and
  from there **all** the points come out wrong by a few hundred mW. Zero is also the
  right reference for the brightness ladder, which is thus read as "what the panel adds
  over black".
- **Points inside the noise are discarded.** The low OPPs add a few milliwatts, of the
  same order as the gauge's dispersion. In the dry-run test the A55 at 300 MHz came out
  at x0.183 against x0.30 for all the rest, simply because 2.5 mW are not measurable
  against σ = 4.3 mA. They are flagged and left out of the summary instead of used
  silently.

It restores governor, frequencies and brightness **always**: from a `finally` and from
the `SIGINT`/`SIGTERM` handlers. If the ssh is cut midway, the phone is not left pinned
at 300 MHz.

It dumps each point to the CSV **as it measures it**, not at the end, because it runs
unplugged over wifi and losing the connection must not cost the run.

And it measures idle **again at the end**. If it has drifted from the first, the run is
contaminated —by heat, by something that woke up, by a cable— and it says so instead of
publishing a calibration built on sand.

### `try-calibrate`

It sets up a fake `/sys` —gauge with the `qcom_qg`'s real noise, energy model with the
surya's real curves, cpufreq with its two policies— and injects some known factors to
check that `calibrate` recovers them.

```sh
./try-calibrate     # ~40 s, no hardware
```

It checks that the governor and the brightness are left as they were, that the CSV
carries all the rows, and that the factors come out within ±0.12 of the injected ones.
**It caught the two faults in the list above** (the baseline moving with the brightness,
and the low-consumption points poisoning the average) before spending 18 minutes of
phone discovering them.

---

## Things that bite

**The uid is 10000, not 1000.** All the cgroup examples on the internet use 1000. Here
the path is `/sys/fs/cgroup/user.slice/user-10000.slice/user@10000.service/`, and
getting it wrong gives no error: it gives an empty app list, which looks like "nothing
is running".

**`current_avg` is broken.** It stayed frozen at `-5035` for a whole 60 s while
`current_now` moved normally. It is tempting to use it for a smooth reading. Do not use
it.

**Discovering the DRM fds costs 79 ms.** Walking all of `/proc/*/fdinfo/*` looking for
`drm-` comes to about 13 mW at 1 Hz — a visible part of idle consumption, spent
precisely by the tool that wants to measure consumption. That is why the PIDs are cached
and only the ~10 files that matter are reread.

**The cgroup disappears when the app is closed.** That is why this has to be a daemon
that samples, and not an app that reads its counters when it opens. An app that opens and
closes between two samplings is invisible: it is the price of sampling, and the reason
the period is 30 s.

**Plugged in nothing can be measured.** It applies to `calibrate` and it applies to the
collector, which discards the whole interval if there was a charger at either of its two
ends. It is the same thing already documented by
[`tasks/015`](../../../tasks/015-battery-gauge.md): "plugged into the PC" is not
"charging".
