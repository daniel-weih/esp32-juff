# Wake acknowledgement

`wake-ack.pcm` contains the fixed Chinese phrase **“我在”** (“I'm here”),
synthesized with Qwen Realtime. It contains no microphone recording or user
conversation. Excess silence was trimmed and the edges faded before exporting
raw signed 16-bit little-endian PCM, mono, 24,000 Hz, without metadata.
The clip is 750 ms long (36,000 bytes).

The host caches the clip on startup and plays it once when a newly awakened
conversation receives `voice.ready`. Playback needs no further speech synthesis
request. Both BLE and Wi-Fi use their existing audio queues and interruption
handling. The reserved `juff-wake-` response prefix keeps local playback events
out of the cloud response lifecycle.
