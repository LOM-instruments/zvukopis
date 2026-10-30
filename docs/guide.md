# zvukopis user guide

*How to record, listen and look after your moments*

zvukopis is a pocket recorder for sonic moments. A moment is a recording: the sound of here and now, kept with its time, a waveform and a tag. This guide covers the whole device. If you only read one section, read "Recording".

## The buttons

- **The side switch.** The rocker on the left edge. Move it up or down to choose, press it in to do the thing, hold it in for more options. Nearly everything is done with it.
- **The Settings button.** The small button next to the USB port. It opens the settings from any screen, and closes them again.
- **The Sleep button.** On the right edge, a little above the side switch. Hold it for a second to send zvukopis to sleep, or to wake it.

The guide at the bottom of every screen says what the switch does right there: "Up or down", "Press in" and "Hold in". Once you know your way around, turn it off in Settings > Button guide and the screens get the room.

## The main screen

The top line shows the date and time, whether the SD card is in, the battery and whether it is charging. The second line counts your moments and says how much recording time is left on the card. The two small buttons at its right end open the library and the calendar.

Below that is the list: *New recording* first, then your moments, newest on top, each with its time, length and waveform. The list always opens on *New recording*, so a press is all it takes.

## Recording

Press the side switch in on *New recording*. The screen shows that it is recording and when it started. Press in again to stop. The moment appears at the top of the list.

A recording can run for up to twelve hours with an SD card in. Without a card the moments live in memory only, which holds a minute or so, and they are gone when the device is switched off.

The screen keeps still while recording, because the panel's refresh is faintly audible to the microphone.

**Mic gain** is in Settings. *Auto* follows the room and is right for most things; a fixed value from 0 to 54 dB is better for a sound you know, such as a concert or a quiet night.

## Quick record

Asleep in your pocket and something is happening? Hold the side switch in. The recording screen comes up and the recording starts right after it, about three seconds from the press. A short knock is ignored. Press in again to stop; the moment is on the list like any other.

## Listening

Move to a moment and press in. It plays through the small speaker, and a playhead runs along the waveform. Press in again to stop.

While it plays, hold the switch up or down and the speed changes like a tape reel: up is faster, down is slower, and past standstill the sound runs backwards. Let go and it eases back to normal. Two times, forwards or backwards, is as far as it goes.

## What else you can do with a moment

Hold the switch in on a moment. A menu opens, titled with the moment's number.

- **Tag it.** The first five entries are the tags: human, nature, funny, art and other, unless you renamed them on the web interface. A tag helps you find the moment later; it does not mean the recording is about that. Choosing the same tag again removes it.
- **Frequency analysis.** A spectrogram of the whole moment, time across and 0 to 20 kHz up, with the loudest point marked and the range that holds most of the sound. Up switches to a spectrum (an FFT graph), down steps through four contrast levels, and press in plays the moment with a playhead on the picture.
- **Delete.** Asks you first. Deleting takes the file off the card, so be sure.
- **Cancel.** Or hold the switch in again.

## The calendar

Move up from *New recording* to the calendar button and press in. Every day with moments has a mark and a count. Choose a day to see only that day's moments; play or hold on them as usual. Hold in to go back.

## The library

zvukopis reads too. Put Markdown or plain text files into the *doc* folder on the card, or upload them on the web interface, and a book button appears next to the calendar. Pick a document and read: up and down turn the pages, press in goes forward, and holding in returns to the library. Each document opens where you left it.

Headings, paragraphs, quotations, lists, italics and bold are laid out; the text is shown in plain ASCII, with typographic quotes and dashes in their plain forms. Files up to half a megabyte, and the first heading becomes the title.

## Sleep and battery

Hold the Sleep button for a second. The screen shows a quote about listening and the device uses almost no power; it wakes the same way. It also goes to sleep by itself after five minutes of nothing, and it wakes by itself for scheduled recordings and for a quick record.

The battery charges whenever the USB cable is plugged in; the lightning mark in the top line shows it. A full battery lasts for days of ordinary use.

If zvukopis ever stops responding, hold the Sleep button for ten seconds: it restarts. Nothing on the card is lost.

## The web interface

Open Settings and choose *Web interface*. The screen shows the address; on a phone or computer on the same Wi-Fi, open http://zvukopis.local (or the address shown).

**The first time**, when zvukopis knows no Wi-Fi network yet, it makes its own: scan the code on the screen with a phone camera, join, and the page opens by itself. Choose your network there and enter its password. From then on zvukopis joins that network when you start the web interface. *Web interface on own hotspot* makes its own network on purpose, for a field without Wi-Fi.

The page has two tabs.

**Recording.** A big Record button, a length to stop after, mic gain. Below it the calendar of days with moments and the moments themselves, each with play, download and delete. *Download all* saves every moment as WAV files in one ZIP, in a folder per day, which is the way to back zvukopis up; a day chosen in the calendar can be downloaded on its own.

**Settings.** Scheduled recordings (a date, a time and a length; zvukopis wakes up for them), the five tags with their names and icons, the clock and the time zone, the library (upload and remove documents) and the Wi-Fi networks it knows.

The web interface stays on until you leave the screen on the device. Wi-Fi uses some battery and can add a little noise to a recording made meanwhile.

## The SD card

Moments are ordinary WAV files (48 kHz, mono, 16-bit) in a folder per day:

> moments/2026-10-28/2026-10-28T10-12-27_nature.wav

The time uses dashes, because FAT names cannot hold colons, and the tag goes at the end. Each file carries a Broadcast Wave timecode of when it started, so an audio editor can place it on the timeline at the right time of day.

**USB card reader.** With the USB cable in a computer, Settings > *USB card reader* turns zvukopis into a card reader: the card appears on the computer, and you can copy moments off or documents on. Press the side switch in to finish; if the computer does not see zvukopis again afterwards, unplug the cable and plug it back in.

**Formatting.** Cards over 32 GB come formatted as exFAT, which zvukopis cannot read. It says so and offers Settings > *Format SD card*, which makes it FAT32. Formatting erases the card.

## Settings, in order

- **Mic gain.** Auto, or a fixed value.
- **Scheduled recordings.** The list of what is planned; they are set up on the web interface.
- **Web interface.** On your Wi-Fi.
- **Web interface on own hotspot.** zvukopis makes its own network.
- **Sync clock.** Sets the clock from the internet, over Wi-Fi.
- **Button guide.** The hints at the bottom of the screens, on or off.
- **USB card reader.** The card on a computer.
- **Format SD card.** Erases it.

Hold the side switch in, or press the Settings button again, to leave.

## If something is odd

- **The screen says it is asleep.** Hold the Sleep button for a second.
- **Nothing happens at all.** Hold the Sleep button for ten seconds to restart.
- **"No SD card" with a card in.** Push the card in until it clicks. A card over 32 GB needs formatting first, see above.
- **The web page does not open.** Make sure the phone or computer is on the same Wi-Fi, and use the address shown on the screen rather than the name. The web interface runs only while that screen is up on the device.
- **A recording sounds faint or distorted.** Set the mic gain back to Auto.

---

*zvukopis is open source: github.com/LOM-instruments/zvukopis. The newest firmware is always on the project page, which installs it from the browser.*
