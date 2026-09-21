# Audio

Sound out and microphone in.

Audio stands on its own and is not part of a window. Everything else the
toolkit owns has a lifetime tied to something on screen and sound does not: a
tool with no window may still want to make one, a phone carries on playing
while the window is in the background, and a machine with two windows still
has one set of speakers.

```c
schultz_audio *audio = NULL;

schultz_audio_create(&audio);
schultz_audio_set_volume(audio, 0.8f);
/* ... */
schultz_audio_destroy(audio);
```

Everything made from the system goes with it, so nothing has to be taken down
in order.

## The one rule

**The host gives bytes. The toolkit never asks for them.**

SDL will call an application back on its audio thread when it wants more data,
and nothing here does that. A host writes a chunk when it has one and asks how
much is still unplayed. That is the same loop in C and in a language with a
garbage collector, and it never puts a host's code on a realtime thread.

Recording is the same rule reversed: the toolkit holds what the microphone
produced and the host reads it when it likes.

## Four things to play

| | What it is for | How it is fed |
|---|---|---|
| A sound | short, played often: a click, an alert | a whole file, decoded once |
| Music | one long piece | a file, decoded as it plays |
| A stream | samples the program works out | the host writes samples |
| A decoder | a file arriving in pieces, which is internet radio | the host writes the file's own bytes |

### A sound

```c
schultz_handle click = SCHULTZ_HANDLE_NONE;

schultz_sound_load_file(audio, "assets/click.wav", &click);
schultz_sound_play(audio, click);
```

Decoded when it is loaded, because a sound decoded again on every press is
work done twice. Eight may play at once, so a run of key presses overlaps
rather than cutting itself off; asking for a ninth answers
`SCHULTZ_ERR_EXHAUSTED` rather than stopping one that can be heard.

`schultz_sound_load_memory` is the same from bytes already in hand.

### Music

```c
schultz_music_play(audio, "assets/theme.ogg", -1);   /* -1 keeps going */
schultz_music_set_volume(audio, 0.5f);
```

One at a time, decoded as it goes rather than held in memory, and with its own
volume: turning the music down while the alerts stay where they were is what
everyone expects of the two.

### A stream: samples the program makes

For a tone, a speech synthesiser, anything generated.

```c
schultz_handle stream = SCHULTZ_HANDLE_NONE;

schultz_audio_stream_create(audio, SCHULTZ_AUDIO_S16, 1, 48000, &stream);
schultz_audio_stream_play(audio, stream);

if (schultz_audio_stream_queued(audio, stream) < want) {
    schultz_audio_stream_write(audio, stream, samples, length);
}
```

The samples are converted and resampled to whatever the device wants, so write
what you have rather than what the hardware asked for.

### A decoder: a file arriving in pieces

The shape internet radio takes. A stream takes samples; this takes the file
itself, in whatever chunks the network hands over.

```c
schultz_handle radio = SCHULTZ_HANDLE_NONE;

schultz_audio_decoder_create(audio, &radio);
schultz_audio_decoder_write(audio, radio, first_chunk, length);  /* then play */
schultz_audio_decoder_play(audio, radio);

/* later, as more arrives */
schultz_audio_decoder_write(audio, radio, chunk, length);
```

Two things to know. **Write before playing**: the decoder reads the start of
the file to see what kind it is, and a few kilobytes is plenty. And an empty
buffer means "not yet" rather than "the end", so a station that goes quiet for
a moment keeps playing when it comes back. Call
`schultz_audio_decoder_finish` when the file has really ended, which is how a
pause is told from an ending.

What has been decoded is dropped as it goes, so a station may play for hours
without the memory growing. `schultz_audio_decoder_queued` says how many bytes
are still waiting, which is how a host decides whether to ask the network for
more yet.

## Stopping, pausing and volume

Each of the four has the same small set, named for what it is:

| | Stop or pause | Is it going | Its own volume |
|---|---|---|---|
| A sound | `schultz_sound_stop` | | `schultz_sound_set_volume` |
| Music | `schultz_music_pause`, `_resume`, `_stop` | `schultz_music_is_playing` | `schultz_music_set_volume` |
| A stream | `schultz_audio_stream_pause`, `_play` | `schultz_audio_stream_is_playing` | `schultz_audio_stream_set_volume` |
| A decoder | `schultz_audio_decoder_pause`, `_play` | `schultz_audio_decoder_is_playing` | `schultz_audio_decoder_set_volume` |

A stream and a decoder resume the way they started, with `_play`. Only music
has a separate `_resume`, because only music has no handle to play again.

Pausing keeps what is queued; `schultz_audio_stream_clear` throws it away,
which is what to call when a program has changed its mind about what it was
about to play rather than wanting to hear the rest of it first.

Every volume is a number from 0 to 1, and every setter has a getter beside it.
They multiply: a sound at 0.5 in a system at 0.8 plays at 0.4.
`schultz_audio_set_volume` moves everything at once, and
`schultz_music_set_volume` is separate so that turning the music down while
the alerts stay where they were is one call.

```c
schultz_audio_set_volume(audio, 0.8f);      /* everything */
schultz_music_set_volume(audio, 0.3f);      /* the music under it */

if (schultz_music_is_playing(audio)) {
    schultz_music_pause(audio);
}
```

**Destroying is optional.** `schultz_sound_destroy`,
`schultz_audio_stream_destroy`, `schultz_audio_decoder_destroy` and
`schultz_audio_recorder_destroy` each free one thing early. Nothing has to be
freed in order, and nothing has to be freed at all: destroying the system
takes everything made from it. Use them for something a program is finished
with long before it exits, like a decoder for a station the listener just left.

## The microphone

```c
schultz_audio_recorder_create(audio, SCHULTZ_AUDIO_S16, 1, 16000, &mic);
schultz_audio_recorder_start(audio, mic);

if (schultz_audio_recorder_available(audio, mic) > 0) {
    schultz_audio_recorder_read(audio, mic, into, length, &read);
}
```

`schultz_audio_recorder_is_running` answers whether it is listening, and
`schultz_audio_recorder_clear` throws away what has been captured but not yet
read, which is what to call before starting a fresh take.

**The platform has to allow it, and the toolkit cannot ask on your behalf.**
iOS refuses without `NSMicrophoneUsageDescription` in the application's
Info.plist and puts its own question to the person; Android needs
`RECORD_AUDIO` in the manifest and a request while running. Where permission
is missing, creating a recorder answers `SCHULTZ_ERR_UNAVAILABLE` rather than
recording silence.

## Choosing a device

Zero means the platform's own choice, which is what a system starts with and
what an application should usually leave alone: it follows the platform when a
headset is plugged in or pulled out, with nobody doing anything.

To offer a choice, list what there is and pick one by its number:

```c
n = schultz_audio_output_count(audio);

for (i = 0; i < n; i++) {
    /* schultz_audio_device_name(audio, i) names it for a person */
}
schultz_audio_set_output(audio, schultz_audio_device_id(audio, 0));
```

Names are good until the next time either count is asked for, which is long
enough to fill in a menu and not long enough to keep. Recording devices are
listed with `schultz_audio_input_count` and chosen with
`schultz_audio_set_input`; the two are kept apart because a headset is often
both and rarely wanted for both.

**What is already open stays where it is.** Choosing a device decides where
the next stream, sound or decoder opens, so move what you want moved by
making it again.

## When the devices change

A headset arriving or leaving changes what an application should be offering.
Asked rather than announced, like everything else here:

```c
if (schultz_audio_devices_changed(audio)) {
    /* fill the menu in again */
}
```

The first call answers yes, because there was nothing to compare against. A
host with a window asks on a turn of its loop; one without asks whenever it
likes.

## Recording to a file

For recording rather than listening. The toolkit writes the samples out as the
device makes them, so nothing is lost while an application is busy elsewhere:

```c
schultz_audio_recorder_save(audio, mic, "note.wav");
schultz_audio_recorder_start(audio, mic);
/* ... */
schultz_audio_recorder_stop_saving(audio, mic);
```

The file is a WAV of exactly what the recorder was asked for. Its header is
written when the file opens and corrected when it closes, so a file left
behind by a crash still opens with whatever had been written by then, and
`schultz_audio_recorder_saved` says how much that is.

**Saving and reading are two ways to the same bytes.** While a recorder is
saving, the toolkit drains it and `schultz_audio_recorder_read` has nothing to
give. Pick one, and `schultz_audio_recorder_is_saving` says which one is
happening.

## What it will play

| Format | What it is good for |
|---|---|
| WAV, AIFF, VOC, AU | uncompressed, and what a short sound should usually be |
| MP3 | the one everything can read |
| Ogg Vorbis | lossy, no patents, a good default for music |
| Opus | lossy, and the best of these at low bit rates; what voice chat uses |
| FLAC | lossless, about half the size of a WAV |
| WavPack | lossless, and a hybrid mode nothing else here has: a small lossy file beside a correction file that restores the original exactly |

Everything but MP3 is decoded by the format author's own library, vendored
and built with the toolkit. MP3 uses a small decoder bundled inside
SDL_mixer, because the alternative is copyleft.

MOD, MIDI and the console formats are deliberately absent: their decoders are
copyleft, which does not suit the commercial license, and MIDI needs a set of
instruments shipped alongside to make any sound at all. AAC is absent because
no decoder for it has a license that fits. `THIRD_PARTY_NOTICES.md` records
the whole of it, and two tests fail if a build ever drifts: one if a copyleft
decoder appears, one if a format on that list stops decoding.

## Sending sound somewhere

The other direction. An encoder takes samples and produces Opus packets, and
what happens to them is the host's business: a socket, a file, a recording.
Nothing here sends anything anywhere.

```c
schultz_handle encoder = SCHULTZ_HANDLE_NONE;
const void *bytes;
uint64_t length, when_ns;

const float *samples = NULL;
uint64_t frames = 0u;

schultz_audio_encoder_create(audio, 1u, 24000u, &encoder);   /* mono, 24 kbps */
schultz_audio_encoder_write(audio, encoder, samples, frames);

while (schultz_audio_encoder_read_packet(audio, encoder, &bytes, &length,
                                         &when_ns) == SCHULTZ_OK) {
    /* send it, write it, keep it */
}
```

Opus codes fixed lengths of time, so samples are held until there are enough
for a packet. Twenty milliseconds is the length used, which is 960 sample
frames at 48000: writing fewer produces nothing yet, and writing more produces
several packets. `SCHULTZ_ERR_EXHAUSTED` means not yet, not a failure.

`schultz_audio_encoder_set_bitrate` is what congestion control is built on.
Sound is the half worth protecting when a network gets bad: a call whose
picture stutters is annoying, and one whose sound breaks up is over.

And the other end of it, for a receiver:

```c
schultz_handle packets = SCHULTZ_HANDLE_NONE;
const float *samples;
uint64_t frames;

const void *bytes = NULL;
uint64_t length = 0u;
schultz_handle stream = SCHULTZ_HANDLE_NONE;

schultz_audio_packet_decoder_create(audio, 1u, &packets);
schultz_audio_packet_decoder_write(audio, packets, bytes, length);
schultz_audio_packet_decoder_read(audio, packets, &samples, &frames);
schultz_audio_stream_write(audio, stream, samples, frames * sizeof(float));
```

Note the two different decoders, which do different jobs.
`schultz_audio_decoder_*` is a player for a whole encoded file -- an MP3, an
Ogg, a FLAC -- arriving in pieces, and it plays it itself.
`schultz_audio_packet_decoder_*` takes bare Opus packets in the order they
were produced, hands back the samples, and plays nothing.

A packet that was lost is written as NULL with a length of zero, and Opus
works out something plausible to cover the gap. That is what keeps a call
going through a bad moment instead of clicking.

## Cleaning up a microphone first

A microphone hears more than the person in front of it, and none of that is
the encoder's problem -- Opus will faithfully encode a room full of echo. See
`schultz_voice.h`:

```c
schultz_voice *voice = NULL;

float *mic = NULL;
const float *played = NULL;
schultz_handle encoder = SCHULTZ_HANDLE_NONE;

schultz_voice_create(960u, 48000u, &voice);       /* 20 ms at 48 kHz */
schultz_voice_clean(voice, mic, played, 960u);    /* in place */
schultz_audio_encoder_write(audio, encoder, mic, 960u);
```

It does four things: takes back out what the speakers played, removes steady
background noise, evens out a quiet or loud talker, and says whether anybody
is speaking. `played` is what went to the speaker at the same moment; passing
NULL turns echo cancellation off for that frame while the rest still runs.

**None of this happens unless you ask for it.** `schultz_audio_recorder_*`
hands over exactly what the microphone captured and has never heard of any of
it. Cleaning up is a step a host puts in between, so raw capture is the
default and there is nothing to turn off.

Each of the four has a switch and a matching getter:

| | switch | default |
|---|---|---|
| Echo cancellation | `schultz_voice_set_echo_removal` / `_echo_removal` | on |
| Noise suppression | `schultz_voice_set_noise_removal` / `_noise_removal` | on |
| Automatic gain | `schultz_voice_set_gain` / `_gain` | on |
| Voice detection | `schultz_voice_set_detection` / `_detection` | **off** |

Detection is off because speexdsp says its own detection is a placeholder
pending a rewrite, and prints a warning when it is switched on. With it off
`schultz_voice_speaking` always answers yes.

How hard each one pushes is a separate struct, so the switches stay simple and
the numbers are all in one place:

```c
schultz_voice *voice = NULL;
schultz_voice_tuning tuning;

schultz_voice_get_tuning(voice, &tuning);
tuning.noise_removal_db = -25.0f;     /* default -15; heavier */
schultz_voice_set_tuning(voice, &tuning);
```

The defaults are speexdsp's own and are what most callers should leave alone:
echo −40 dB (−15 while this end is talking), noise −15 dB, gain aiming for
0.244 of full scale with a 30 dB ceiling, detection starting at 35% and
holding at 20%. `schultz_voice_tuning_default` fills them in.

Values out of range are brought into range rather than refused, and a
reduction written as a positive number is taken as meant — so
`schultz_voice_get_tuning` always reports what is actually in force, not what
was passed.

It works one fixed frame length at a time, set when the voice is created,
because the echo canceller keeps a running picture of the room that only makes
sense at one size. Mono: echo cancellation on a stereo microphone is a
different and much harder problem, and a call has one voice in it.

## Sound that comes with a picture

A video node plays its own sound track through this same system, and it needs
to be told where the system is:

```c
schultz_tree_set_audio(tree, audio);
```

Once a tree has been given one, any video node in it that carries an Opus
track is heard. Without one the film plays silently rather than refusing. See
the Video section of `docs/widgets.md`.

## What it is not

There is no mixing graph, no effects, no filters, no three dimensional
placement, and no synthesis. An application that wants those is writing an
audio engine: in C it can link SDL_mixer itself, and from another language it
should ask for what it needs to be added here.

The demo's Basics page has all three roads into playback, so the difference
between them can be heard rather than only read.
