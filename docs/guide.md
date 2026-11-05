# Zvukopis user guide

*How to record, listen and look after your moments*

Zvukopis is a pocket recorder for sonic moments. A moment is a recording: the sound of here and now, kept with its time, a waveform and a tag. This guide covers the whole device. If you only read one section, read "Recording".

## The buttons

- **The side switch.** The rocker on the left edge. Move it up or down to choose, press it in to do the thing, hold it in for more options. Nearly everything is done with it.
- **The Settings button.** The small button next to the USB port. It opens the settings from any screen, and closes them again.
- **The Sleep button.** On the right edge, a little above the side switch. Hold it for a second to send Zvukopis to sleep, or to wake it.

The guide at the bottom of every screen says what the switch does right there: "Up or down", "Press in" and "Hold in". Once you know your way around, turn it off in Settings > Button guide and the screens get the room; the pointers on the sleep screen go with it.

## The main screen

The top line shows the date and time, whether the SD card is in, the battery and whether it is charging. The second line counts your moments and says how much recording time is left on the card. The two small buttons at its right end open the library and the calendar.

Below that is the list: *New recording* first, then your moments, newest on top, each with its time, length and waveform. The list always opens on *New recording*, so a press is all it takes.

## Recording

Press the side switch in on *New recording*. The screen shows that it is recording and when it started. Press in again to stop. The moment appears at the top of the list.

A recording can run for up to six hours with an SD card in. Without a card the moments live in memory only, which holds about half a minute, and they are gone when the device is switched off.

The screen keeps still while recording, because the panel's refresh is faintly audible to the microphone.

**Mic gain** is in Settings. *Auto* follows the room and is right for most things; a fixed value from 18 to 54 dB is better for a sound you know, such as a concert or a quiet night.

## How Auto gain works

Auto gain is a simple, deliberate thing, not a limiter. It moves the microphone gain in steps while a recording runs, and it only ever reacts to what it has already heard.

- It starts a recording at the gain the last one ended on (36 dB the first time), and it can go from 18 to 54 dB.
- Sixty times a second it looks at the loudest sample of the last 16 milliseconds. If that came within 3 dB of clipping, the gain steps down at once: by 3 dB while it is below 30 dB, by 6 dB above. The loud sound stays in the recording, a little softer from that instant on.
- If everything stays below a fifth of full scale (-20 dB) for one and a half seconds without a break, the gain starts climbing, steadily, about 3 dB per second, faded by the codec itself so there are no steps to hear, until a sound holds it or it reaches the top. A single louder sound in that time starts the count again, so the gain climbs only in a true lull.
- Anything in between leaves it alone.

The step down is instant on purpose: it is there to stop clipping, and the loud sound covers it. The changes are kept in the file as recorded. Each file says in its header which gain it ended on, marked *auto*. For music, for anything where the dynamics matter, or for comparing one recording with another, set a fixed gain instead: the level then stays put, and clipping is the signal to choose a lower one next time.

## Quick record

Asleep in your pocket and something is happening? Hold the side switch in. The recording screen comes up and the recording starts right after it, about three seconds from the press. A short knock is ignored. Press in again to stop; the moment is on the list like any other.

## Listening

Move to a moment and press in. It plays through the small speaker, and a playhead runs along the waveform. Press in again to stop.

While it plays, hold the switch up or down and the speed changes like a tape reel: up is faster, down is slower, and past standstill the sound runs backwards. Let go and it eases back to normal, or, if you prefer, the speed stays where you left it and a tap of the switch brings it back. How far it goes (two to six times), how quickly, and what letting go does is *Varispeed* in Settings, on the device or on the web interface.

## What else you can do with a moment

Hold the switch in on a moment. A menu opens, titled with the moment's number.

- **Tag it.** The first entries are the tags: sound, music, note, biophony and anthropophony, unless you renamed them or added your own on the web interface (up to twelve; with many, the list scrolls). A tag helps you find the moment later; it does not mean the recording is about that. Choosing the same tag again removes it.
- **Frequency analysis.** A spectrogram of the whole moment, time across and 0 to 20 kHz up, with the loudest point marked and the range that holds most of the sound. Up switches to a spectrum (an FFT graph), down steps through four contrast levels, and press in plays the moment with a playhead on the picture.
- **Delete.** Asks you first. Deleting takes the file off the card, so be sure.
- **Cancel.** Or hold the switch in again.

## The calendar

Move up from *New recording* to the calendar button and press in. Every day with moments has a mark and a count. The calendar needs the clock: a moment recorded while the clock isn't set has no date and no day, and the calendar says so. Choose a day to see only that day's moments; play or hold on them as usual. Hold in to go back.

## The library

Zvukopis reads too. Put Markdown or plain text files into the *doc* folder on the card, or upload them on the web interface, and a book button appears next to the calendar. Pick a document and read: up and down turn the pages, press in goes forward, and holding in returns to the library. Each document opens where you left it.

Headings, paragraphs, quotations, lists, italics and bold are laid out; the text is shown in plain ASCII, with typographic quotes and dashes in their plain forms. Files up to half a megabyte, and the first heading becomes the title.

## Sleep and battery

Hold the Sleep button for a second. The screen shows a quote about listening and the device uses almost no power; it wakes the same way. It also goes to sleep by itself after five minutes of nothing, and it wakes by itself for scheduled recordings and for a quick record.

The battery charges whenever the USB cable is plugged in; the lightning mark in the top line shows it. A full battery lasts for days of ordinary use.

If Zvukopis ever stops responding, hold the Sleep button for ten seconds: it restarts. Nothing on the card is lost.

## The web interface

Open Settings and choose *Web interface*. The screen shows the address; on a phone or computer on the same Wi-Fi, open http://zvukopis.local (or the address shown).

**The first time**, when Zvukopis knows no Wi-Fi network yet, it makes its own: scan the code on the screen with a phone camera, join, and the page opens by itself. Choose your network there and enter its password. From then on Zvukopis joins that network when you start the web interface. *Web interface on own hotspot* makes its own network on purpose, for a field without Wi-Fi.

The page has two tabs.

**Recording.** A big Record button, a length to stop after, mic gain. Below it the calendar of days with moments and the moments themselves, each with play, download and delete. *Download all* saves every moment as WAV files in one ZIP, in a folder per day, which is the way to back Zvukopis up; a day chosen in the calendar can be downloaded on its own.

**Settings.** Scheduled recordings (a date, a time and a length; Zvukopis wakes up for them), the tags with their names and icons (add more, up to twelve, or remove the last), varispeed, the clock and the time zone, the library (upload and remove documents) and the Wi-Fi networks it knows.

The web interface stays on until you leave the screen on the device. Wi-Fi uses some battery and can add a little noise to a recording made meanwhile.

## The SD card

Moments are ordinary WAV files (96 kHz, mono, 16-bit) in a folder per day:

> moments/2026-10-28/2026-10-28T10-12-27_nature.wav

The time uses dashes, because FAT names cannot hold colons, and the tag goes at the end. Each file carries a Broadcast Wave timecode of when it started, so an audio editor can place it on the timeline at the right time of day.

**USB card reader.** With the USB cable in a computer, Settings > *USB card reader* turns Zvukopis into a card reader: the card appears on the computer, and you can copy moments off or documents on. Eject the drive on the computer when you are done and Zvukopis restarts into its main screen by itself (pressing the side switch in does the same).

**USB microphone.** With the USB cable in a computer or a tablet, Settings > *USB microphone* turns Zvukopis into a microphone for it: it appears as "Zvukopis microphone", mono, 16-bit, 96 kHz, with nothing to install (macOS, Windows 10 and later, Linux and iPadOS have the driver; Android phones vary). Up and down on the side switch set the mic gain, Auto at the bottom, and the choice stays as the recording gain. On a Mac, set the input to 96 kHz, 16-bit in Audio MIDI Setup; at another setting the recording comes out wrong. The screen holds still while the microphone is on, because a refresh is heard in it, so it redraws only when the gain changes. Press the side switch in to leave; Zvukopis restarts.

**Formatting.** Cards over 32 GB come formatted as exFAT, which Zvukopis cannot read. It says so and offers Settings > *Format SD card*, which makes it FAT32. Formatting erases the card.

## Settings, in order

- **Mic gain.** Auto, or a fixed value.
- **Varispeed.** The top speed, the ramp, and whether letting go brings the speed back or keeps it.
- **Scheduled recordings.** The list of what is planned; they are set up on the web interface.
- **Web interface.** On your Wi-Fi.
- **Web interface on own hotspot.** Zvukopis makes its own network.
- **Sync clock.** Sets the clock from the internet, over Wi-Fi.
- **Button guide.** The hints at the bottom of the screens, on or off.
- **USB card reader.** The card on a computer.
- **USB microphone.** Zvukopis as a microphone for the computer.
- **Format SD card.** Erases it.
- **Firmware update.** Which version this is, and how to install the newest (a code to scan, three steps).
- **Reset settings.** Forgets the Wi-Fi networks, tags, schedule, gain and the rest; the moments on the card stay. For handing Zvukopis on.

Hold the side switch in, or press the Settings button again, to leave.

## If something is odd

- **The screen says it is asleep.** Hold the Sleep button for a second.
- **Nothing happens at all.** Hold the Sleep button for ten seconds to restart.
- **"No SD card" with a card in.** Push the card in until it clicks. A card over 32 GB needs formatting first, see above.
- **The web page does not open.** Make sure the phone or computer is on the same Wi-Fi, and use the address shown on the screen rather than the name. The web interface runs only while that screen is up on the device.
- **A recording sounds faint or distorted.** Set the mic gain back to Auto.

---

*Zvukopis is open source: github.com/LOM-instruments/zvukopis. The newest firmware is always on the project page, which installs it from the browser.*
