# What's in a Name: Stellyra

## Pronunciation

Both of the following are valid:

- **stel-EYE-ruh** - /stɛˈlaɪ.rə/ - the second half follows the same sound as *Lyra* (LY-ruh), not *lyre* said on its own.
- **stel-EER-ruh** - /stɛˈlɪərə/ - the second half rhymes with "ear" instead, a deliberate divergence from *Lyra*'s own pronunciation rather than a mistake.

## The two roots

**Stellyra** comes from two pieces:

- ***Stella*** - Latin for "star."
- ***Lyra*** - the constellation of the lyre, one of the smallest but brightest constellations in the night sky, home to Vega, the fifth-brightest star visible from Earth.

Put together, the name points at a specific image: **a harp made of stars.**

## The myth behind Lyra

In Greek myth, the lyre belonged to Orpheus, the musician whose playing was said to be powerful enough to calm wild animals, move rivers, and talk the god of the underworld into releasing his dead wife, Eurydice.  After Orpheus died, the gods placed his lyre among the stars - it became the constellation Lyra.

That's the half of the name that gives Stellyra its *instrument*.  The half that gives it its *mechanism* comes from physics, not myth.

## The mechanism: sympathetic resonance

Pluck one string on a harp or lyre, and nearby strings tuned to related frequencies will begin to vibrate on their own - without ever being touched.  The energy travels invisibly, through the air, from the string that was struck to every other string that happens to be tuned to listen for it.  This is called **sympathetic resonance**, and it's a real, physical phenomenon, not just a figure of speech.

That's the mechanism Stellyra is named for:

- An event fires - a string is plucked.
- A handler has to be tuned to that event first - that's what `connect()` does.  Without that step, nothing resonates; the tuning doesn't happen on its own.
- Once tuned, though, the response itself happens without the emitter and handler touching directly: the emitter plucks its string and lets the vibration carry outward, rather than reaching in and invoking each handler by hand.
- At the moment it fires, the emitter doesn't need to know who, if anyone, is listening.  The handler doesn't need to touch the emitter back.  Once connected, they're linked only by being tuned to the same frequency.
- One pluck can set off every tuned string at once, synchronously or with its own decay over time - which is exactly the shape of an event reaching zero, one, or many handlers, sync or async, at once.

A system built this way behaves like a single instrument, even though no two strings ever touch each other directly once they're tuned.  That's the whole idea of an event/handler library in one image.

## The invented definition

Since *Stellyra* isn't a real word, here's the definition it's meant to carry going forward:

> **stellyra** *(n.)* - a connection by which what happens to one thing is felt by another, however far apart they are; a bond unaffected by distance.

## Why it fits the library

The library uses a few different naming conventions for the same two roles. Stellyra maps to both sides of each:

| Role | Also known as | Stellyra concept |
|---|---|---|
| The thing that fires | Event, Signal, Emitter, Publisher | The string that is struck - the pluck itself |
| The thing that responds | Handler, Slot, Receiver, Subscriber | A string tuned to resonate in response |

## In code

```cpp
stellyra::Event<int> onReading;
stellyra::Signal<int> onReadingAlias;  // Signal is an alias for Event

onReading.connect(handler, Handler::method);
onReading.trigger(42);  // one pluck, every tuned string responds
```
