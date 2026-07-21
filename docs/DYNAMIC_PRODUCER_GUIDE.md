# KickLock Dynamic Mode — Producer Guide

## What a State is

A **State** is a repeatable kick/bass conflict that KickLock has recognized -
not a MIDI note, not a timeline region. The same bass note played the same way
against the same kick pattern is one State; play it differently and it may
form a different State (or none, if it never repeats). Pitch and MIDI shown
next to a State are optional, informational only.

## Learn workflow

1. Play or loop the full bass phrase you want corrected, with the kick
   sidechain routed in.
2. Run **Learn**. KickLock listens for repeatable kick/bass conflicts.
3. Learn produces a **Preview** - recognized States and any automatic
   corrections it's confident in. Preview is read-only: nothing is applied
   yet, nothing plays differently, and Verified always shows unavailable
   here because nothing has actually run through the audio yet.
4. **Apply** to make the result live. **Discard** to throw the preview away
   without touching whatever was applied before.

## After Apply

Every recognized State gets one of these statuses:

- **Automatic correction** - Learn found a package (Delay/Frequency/Q) that
  reliably helps, and it's live.
- **Recognized, no automatic correction** - KickLock knows this conflict
  exists but couldn't find a package it trusted enough to apply on its own.
  It uses Global for this State. You can finish it manually (see below).
- **Global-only** - not enough repeatable evidence yet to be a real State.

## Manual editing

Select a State to inspect it. If it doesn't have an automatic correction, hit
**Promote to Manual** to start editing it (this requires at least 3
repeatable hits - a one-off event can't become a permanent State). Then drag
**Delay**, **Frequency**, or **Q** to taste. Changes apply live while you
work, without clicking, even if that State is actively playing back at the
time - KickLock momentarily uses an internal bridge branch to bring in your
new setting cleanly, then hands off to the permanent slot once it's safe to
do so silently. You won't hear the mechanics, just a clean update.

Other actions:

- **Reset Manual Trim** - undo your adjustments, keep the learned/base
  package.
- **Reset to Learned** (Auto States) - back to exactly what Learn found.
- **Reset to Global** (Auto States) - keep the State recognized, but stop
  correcting it; Global applies instead.
- **Enable / Disable** - a disabled State is not matched at all.
- **Bypass / Unbypass** - a bypassed State is still recognized, but
  deliberately uses Global.
- **Promote to Manual** / **Remove Manual State** - removal asks for
  confirmation; it can't be undone by accident.

## Focus

Select a State and turn on **Focus** to watch it specifically: KickLock
reports when that State is detected, how much fresh verification it has
collected, and whether it's currently disabled/bypassed/not showing up. Focus
never changes what's playing - the whole mix keeps running normally, and
other States are never muted or forced.

## Predicted vs. Verified

**Predicted** comes from the material Learn analyzed offline - a forecast.
**Verified** comes from actually processed audio, only once a State has
settled into being genuinely audible a few times in a row. They're never the
same number and are never shown as if they were. Editing a State's package
resets its Verified evidence - the old number was measuring the *old*
package, so it can't keep being shown as if it verified the new one.

## Recent Unknowns

If KickLock hears something it doesn't recognize as a clean match, but it
keeps happening, it shows up under Recent Unknowns. From there you can
**Create Manual State** (once it's repeated enough) or **Ignore** it. A
single one-off unrecognized hit is never turned into a permanent State on
its own.

## Loop, save, reload

Corrections survive looping - the first kick after a loop wraps is corrected
normally, with no forced reset to Global. Save/reload restores every Manual
and Auto-trim edit exactly; Verified always restarts as unavailable after a
reload since it's fresh runtime evidence, not something saved.

## Revert

**Revert** restores the exact previous applied map - every package, trim,
origin, enabled/bypassed flag - as it was before your most recent Apply or
edit session. It does not restore runtime selection history or Verified
data; those rebuild fresh from whatever map is active.

## Troubleshooting: unstable bass phase

If Learn reports **"KickLock detected a non-repeatable phase relationship"**:
the same bass note isn't hitting the kick the same way twice, so no stable
correction can be built for it. Common causes: free-running oscillator phase,
phase randomization, unison beating, or heavy modulation. Try:

- Disabling oscillator phase randomization, or enabling phase retrigger.
- Rendering/freezing the bass part to audio before Learning.
- Reducing random modulation or unison width.
- Running Learn again.

This doesn't mean correction is impossible in general - a bass part with
multiple genuinely repeatable phase families can still form separate States.
