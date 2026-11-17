# Zvukopis user guide

*How to record, listen and look after your moments*

Zvukopis is a pocket recorder for sonic moments. A moment is a recording: the sound of here and now, kept with its time, a waveform and a tag. This guide covers the whole device.

## Parts and controls

1. **The side switch.** The rocker on the left edge. Move it up or down to choose, press it in to confirm the choice, and hold it in for more options. This is the main controller of the device.
2. **The microphone.** A small hole at the bottom of the device, to the left of the USB-C port. Zvukopis records through it, so keep it uncovered while you record.
3. **The USB-C port.** The socket in the middle of the bottom edge. It charges the battery, and it connects Zvukopis to a computer: for installing the firmware, and for using Zvukopis as a card reader or as a USB microphone.
4. **The microSD card slot.** The slot at the bottom of the device, to the right of the USB-C port. Push the card in until it clicks. The moments are kept on the card, and so are the documents of the library.
5. **The Settings button.** The small button on the right side of the device marked with a cog icon on the case. Press it to open Settings, and press it again to leave.
6. **The Sleep button.** The small button on the right side of the device marked with a moon icon on the case. It is placed above the Settings button.

The button guide at the bottom of every screen says what the switch does right there: "Up or down", "Press in" and "Hold in". Once you know your way around, you can turn it off in Settings > *Button guide*, which leaves that space to the screens themselves. The two pointers on the sleep screen, which show how to wake Zvukopis and how to start a quick record, are turned off with it.

## The main screen

The top line shows the date and time, whether the SD card is in, the battery and whether it is charging. The second line counts your moments and says how much recording time is left on the card. The two small buttons at its right end open the text library and the calendar.

Below that is the list: *New recording* first, then your moments, newest on top, each with its time, length and waveform. The list always opens on *New recording*, so one press of the side switch is enough to start a recording.

_Now and then the whole screen flashes black and white for a moment. That is the e-paper cleaning itself, and it is normal: it happens a little while after waking, during long playback and every few pages in the library._

## Recording

To start a recording, press the side switch in on *New recording*. The screen then shows that Zvukopis is recording and when the recording started. Press the switch in again to stop, and the new moment appears at the top of the list.

The microphone is positioned at the bottom of the device, next to the USB-C port.

A recording can run for up to six hours with an SD card in. Without a card the moments live in memory only, which holds about half a minute, and they are gone when the device restarts or is switched off. The list holds up to 500 moments; at 500 it says *list full* and nothing is recorded until some are deleted or moved off the card.

The screen keeps still while recording, because the panel's refresh interference is faintly audible to the microphone.

**Mic gain** sets how much the microphone's signal is amplified, and you find it in Settings. *Auto* follows the loudness of the environment and is right for most things; a fixed value from 18 to 54 dB is better for a sound you know, such as a concert or a quiet night.

## How Auto gain works

Auto gain is deliberately simple, and it is not a limiter. It moves the microphone gain in steps while a recording runs, and it only ever reacts to what it has already heard.

- It starts a recording at the gain the last one ended on (36 dB the first time after a restart), and it can go from 18 to 54 dB.
- Sixty times a second it looks at the loudest sample of the last 16 milliseconds. If that came within 3 dB of clipping (the point where a sound is too loud for the recorder and comes out distorted), the gain steps down at once: by 3 dB while it is below 30 dB, by 6 dB above. The loud sound stays in the recording, a little softer from that instant on.
- If everything stays below a tenth of full scale (-20 dB) for one and a half seconds without a break, the gain starts climbing, steadily, about 3 dB per second, faded by the codec itself so there are no steps to hear, until a sound holds it or it reaches the top. A single louder sound in that time starts the count again, so the gain climbs only in a true lull.
- At any level in between, the gain stays where it is.

The step down is instant on purpose: it is there to stop clipping, and the loud sound covers it. The changes are kept in the file as recorded. Each file says in its header which gain it ended on, marked *auto*. For music, for anything where the dynamics matter, or for comparing one recording with another, set a fixed gain instead: the level then stays put, and clipping is the signal to choose a lower one next time.

## Quick record

If Zvukopis is asleep in your pocket and you want to quickly start recording, hold the side switch in for about a second. The recording screen comes up and the recording starts right after it, about two seconds from the press. A short push is ignored. Press in again to stop; the moment is on the list like any other.

## Listening

To listen to a moment, move to it in the list and press the side switch in. It plays through the small speaker at the back of the device, and a playhead runs along the waveform. Press in again to stop.

While it plays, hold the switch up or down and the speed changes like a tape reel: up is faster, down is slower, and past standstill the sound runs backwards. Let go and it eases back to normal, or, if you prefer, the speed stays where you left it and a tap of the switch brings it back. How far the speed can go (up to six times, or never above normal), how quickly it changes, and what happens when you let go are all set under *Varispeed* in Settings, on the device or on the web interface.

## What else you can do with a moment

Hold the switch in on a moment to open its menu, which is titled with the moment's number.

- **Tag it.** The first entries are the tags: sound, music, note, biophony (the sounds of living things) and anthropophony (the sounds people make), unless you renamed them or added your own on the web interface (up to twelve; with many, the list scrolls). A tag helps you find the moment later; it does not mean the recording is about that. Choosing the same tag again removes it.
- **Frequency analysis.** This opens a spectrogram of the whole moment: a picture of the sound in which time runs from left to right, frequency from 0 kHz at the bottom to 48 kHz at the top, and darker ink means a louder sound. Up and down step through four contrast levels, and pressing in plays the moment with a playhead on the picture. Hold in for more: the spectrum (an FFT graph, which shows how loud each frequency is on average over the whole moment), a scale of 0 to 24 or 0 to 12 kHz (which suits birdsong and speech, and stays until you change it), or the way back to the moments.
- **Delete.** Zvukopis asks you to confirm first, because deleting takes the file off the card and cannot be undone.
- **Cancel.** This closes the menu without changing anything. Holding the switch in again does the same.

## The calendar

To open the calendar, move up from *New recording* to the calendar button and press in. Every day with moments is marked with dots, one for each moment and four at most, and under the month you can read how many moments the chosen day has and how long they are in total. The calendar needs the clock: a moment recorded while the clock isn't set has no date and no day, and the calendar says so. Up and down move between the days that have moments, and pressing in opens a day and shows only that day's moments; you can play them, or hold the switch in on one for its menu, as on the main screen. To leave a day, move up to *Back* at the top of the screen and press in. In the calendar itself, holding the switch in takes you back to the main screen.

## The library

Zvukopis can also be used as an e-reader. Put Markdown (.md) or plain text files (.txt) into the `/doc/` folder on the card, or upload them on the web interface, and they appear in the library. To open it, move up from *New recording* to the book button next to the calendar and press in. With a card in, this guide is always in the library: it comes with the firmware, which keeps its own copy in that folder and puts it back as it was at every start. Pick a document and read: up and down turn the pages, press in goes forward, and holding in returns to the library. Each document opens where you left it.

Headings, paragraphs, quotations, lists, italics and bold are laid out; the text is shown in plain ASCII, with typographic quotes and dashes in their plain forms. A few accented letters lose their accent, and other letters and signs outside plain English show as question marks. Files can be up to half a megabyte, and the first heading becomes the title of the book.

## Sleep and battery

To put Zvukopis to sleep, hold the Sleep button for a second. The screen shows a quote about listening by Pauline Oliveros and the device uses almost no power; it wakes the same way. It also goes to sleep by itself after five minutes without use, though never while it records or plays. A scheduled recording starts by itself even while Zvukopis sleeps, and holding the side switch in wakes it for a quick record.

The battery charges whenever the USB cable is plugged in; the lightning mark in the top line shows it. A full battery lasts for days of ordinary use. During testing, we have found that the pure recording time is at least 35 hours. When the battery is nearly empty (about 3 %), Zvukopis saves a recording that is running, says so on the screen and switches itself off. Once you plug in the USB cable, it starts again by itself and charges.

If Zvukopis ever stops responding, hold the Sleep button for ten seconds: it restarts. Nothing on the card is lost; a recording that was running keeps all but its last few seconds.

## The web interface

The web interface is a page that Zvukopis itself offers over Wi-Fi, to a phone or a computer: on it you can record remotely, download your moments and change the settings. To start it, open Settings and choose *Web interface*. The screen shows the address and a QR code for it; on a phone or computer on the same Wi-Fi, scan the code or open the address as shown. The address ends in a key, a new one every time the web interface starts, so the page opens only for someone who can see the screen.

**The first time**, when Zvukopis knows no Wi-Fi network yet, it makes its own. Select *Web interface* in Settings, scan the code on the screen with a phone camera, join, and the page opens by itself. Choose your Wi-Fi network there and enter its password. From then on Zvukopis joins that network when you start the web interface. *Web interface on own hotspot* makes its own network on purpose, for a field without Wi-Fi.

The page has two tabs.

**Recording.** This tab has a big Record button, a choice of the length after which the recording stops by itself, and the mic gain. Below them are the calendar of days with moments and the moments themselves, each of which can be played, downloaded or deleted. *Download all* saves every moment as WAV files in one ZIP, in a folder per day, which is the way to back Zvukopis up; a day chosen in the calendar can be downloaded on its own. One ZIP holds up to 4 GB, a little over six hours of recordings; when there is more than that, the page offers the days one at a time.

**Settings.** This tab is where recordings are scheduled: each one has a date, a time and a length, and Zvukopis wakes up for it. The tab also holds the tags with their names and icons (you can add more, up to twelve, or remove the last one), varispeed, the clock and the time zone, the library (where you upload and remove documents) and the Wi-Fi networks Zvukopis knows.

The web interface stays on until you press the side switch in on the device, or until ten minutes pass with nothing done on the page; a recording that is running keeps it on. Wi-Fi uses some battery and can add a little noise to a recording made meanwhile.

## The SD card

Moments are ordinary WAV files (96 kHz, mono, 16-bit) in a folder per day:

> moments/2026-10-28/2026-10-28T10-12-27_nature.wav

The time goes `YYYY-MM-DDTHH-MM-SS`: it uses dashes, because FAT names cannot hold colons, and the tag goes at the end. Each file carries a Broadcast Wave timecode of when it started, so an audio editor can place it on the timeline at the right time of day.

**USB card reader.** With the USB cable in a computer, Settings > *USB card reader* turns Zvukopis into a card reader: the card appears on the computer, and you can copy moments off or documents on. Eject the drive on the computer when you are done and Zvukopis restarts into its main screen by itself (pressing the side switch in does the same; eject first, or the computer may not have finished writing).

**Big files.** For long recordings, take the card out and use an external card reader. Wi-Fi and the USB card reader above are not very fast: an hour of recording is about 700 MB, which takes a quarter of an hour over USB and longer over Wi-Fi.

**Formatting.** Cards over 32 GB come formatted as exFAT, which Zvukopis cannot read. Zvukopis says so in Settings and offers *Format SD card* there, which makes the card FAT32. Formatting erases everything on the card, so copy off what you want to keep first.

## USB microphone

With the USB cable in a computer or a tablet, Settings > *USB microphone* turns Zvukopis into a microphone for it: it appears as "Zvukopis microphone", mono, 16-bit, 96 kHz, with nothing to install (macOS, Windows 10 and later, Linux and iPadOS have the driver; Android phones vary). Up and down on the side switch set the mic gain, with *Auto* as the last step down, and the gain chosen here stays as the mic gain for recordings too. On a Mac, set the input to 96 kHz, 16-bit in Audio MIDI Setup; at another setting the recording comes out wrong. The screen holds still while the microphone is on, because a refresh is heard in it, so it redraws only when the gain changes. Press the side switch in to leave; Zvukopis restarts.

## Settings, in order

- **Mic gain.** Sets how much the microphone's signal is amplified: *Auto*, or a fixed value from 18 to 54 dB in steps of 6 dB. A low value suits loud sounds and a high one quiet sounds.
- **Varispeed.** Sets how the playback speed behaves when you hold the switch up or down: the top speed (1, 2, 3, 4 or 6 times; at 1 the sound never runs faster than normal, so the switch only slows it down and turns it backwards), the ramp (how quickly the speed changes) and whether letting go brings the speed back to normal or keeps it.
- **Scheduled recordings.** Opens the list of the recordings that are planned; the clock has to be set for them. *Schedule a recording* adds one: you set its day, its time, its length and whether it happens once or every day. To change a value, press in on it, move the switch up or down (holding it goes faster) and press in again; when everything is right, choose *Save*. In the list, pressing in on a planned recording lets you change it, and holding in removes it; Zvukopis asks first. Recordings can also be scheduled on the web interface, where the time can be set to the second.
- **Web interface.** Starts the web interface on your Wi-Fi network, so that a phone or a computer on the same network can open its page. When no known network is in range, it starts on its own hotspot instead.
- **Web interface on own hotspot.** Starts the web interface on a network that Zvukopis makes itself, for places without Wi-Fi.
- **Sync clock.** Sets the clock from the internet, over Wi-Fi. Zvukopis also does this by itself at start-up when its clock was lost (the battery was empty or unplugged) and it knows a Wi-Fi network. The time zone is Central European until you set another one, on the web interface or with the *Set the clock* button on the project page.
- **Button guide.** Turns the hints at the bottom of the screens on or off. The hints say what the side switch and the two buttons do on the screen you are looking at.
- **USB card reader.** Turns Zvukopis into a card reader, so that the SD card appears as a drive on a computer connected with the USB cable.
- **USB microphone.** Turns Zvukopis into a microphone for a computer or a tablet connected with the USB cable.
- **Format SD card.** Formats the card as FAT32, which a card over 32 GB needs before Zvukopis can read it. Formatting erases everything on the card, so Zvukopis asks first.
- **Firmware update.** Shows which firmware version Zvukopis is running and how to install the newest one: a code to scan, which leads to the project page, and three steps to follow.
- **Reset settings.** Makes Zvukopis forget the Wi-Fi networks, the tags, the schedule, the gain and all its other settings, and then restart; it asks first, and the moments on the card stay. Use it before you hand Zvukopis on to someone else, because until then its saved Wi-Fi passwords are still in it.

Hold the side switch in, or press the Settings button again, to leave.

## Troubleshooting

- **The screen says it is asleep.** Hold the Sleep button for a second to wake Zvukopis up.
- **Nothing happens at all.** Hold the Sleep button for ten seconds to restart.
- **"No SD card" with a card in.** Push the card in until it clicks. A card put in while Zvukopis is on is not noticed at once: Zvukopis looks for it when you open Settings, start a recording or play a moment. A card over 32 GB has to be formatted first; see "Formatting" under "The SD card".
- **The web page does not open.** Make sure the phone or computer is on the same Wi-Fi as Zvukopis, and use the first address shown on the screen, the one made of numbers, complete with the key at its end. The second address, which has a name with .local in it, does not work on every network. The key is new at every start, so a page left open from an earlier start has to be opened again at the new address. Also, the web interface runs only while its screen is up on the device.
- **A recording sounds faint or distorted.** The mic gain was probably fixed at a value too low for the sound (faint) or too high (distorted). Set it back to *Auto* in Settings.
- **The whole screen flashes black and white.** That is the e-paper cleaning itself, see "The main screen". It is normal.
- **The screen is slow in the cold.** Under about ten degrees Celsius a full change of the screen takes five seconds and more, and near freezing it may lag far behind. Recording carries on regardless, so give it time before pressing again. Also keep the screen out of direct sun and hot cars: the panel is made for temperatures up to 50 degrees Celsius.

---

*Zvukopis is open source: github.com/LOM-instruments/zvukopis. The newest firmware is always on the project page, zvukopis.lom.audio, which installs it from the browser.*
