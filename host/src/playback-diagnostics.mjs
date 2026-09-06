// Keep counts and timing only. Audio and transcripts never enter these logs.
export class PlaybackDiagnostics {
  constructor({ now = Date.now, report = () => {} } = {}) {
    this.now = now
    this.report = report
    this.responses = new Map()
  }

  clear() { this.responses.clear() }

  source(event) {
    if (event?.type === 'playback.clear' || event?.type === 'voice.deactivated') {
      this.clear()
      return
    }
    if (!['audio.delta', 'audio.done'].includes(event?.type)
      || typeof event.responseId !== 'string' || !event.responseId) return
    let response = this.responses.get(event.responseId)
    if (!response) {
      if (this.responses.size >= 32) this.responses.delete(this.responses.keys().next().value)
      response = { pcmBytes: 0, lastDeltaAt: null, maxSourceGapMs: 0, sourceDone: false }
      this.responses.set(event.responseId, response)
    }
    if (event.type === 'audio.done') response.sourceDone = true
    else if (typeof event.audio === 'string') {
      const now = this.now()
      if (response.lastDeltaAt !== null) {
        response.maxSourceGapMs = Math.max(response.maxSourceGapMs, now - response.lastDeltaAt)
      }
      response.lastDeltaAt = now
      response.pcmBytes += Buffer.byteLength(event.audio, 'base64')
    }
  }

  device(event) {
    if (event?.type !== 'speaker.credit') return
    const response = this.responses.get(event.responseId)
    if (!response || !Number.isSafeInteger(event.received) || event.received < 0
      || !Number.isSafeInteger(event.underruns) || event.underruns < 0
      || !Number.isSafeInteger(event.dropped) || event.dropped < 0) return
    response.firstUnderruns ??= event.underruns
    response.lastUnderruns ??= event.underruns
    response.firstDropped ??= event.dropped
    const terminal = ['ended', 'cancelled'].includes(event.state)
    const starved = event.underruns > response.lastUnderruns
    response.lastUnderruns = event.underruns
    if (!terminal && !starved) return
    // Current transport: mono 24 kHz PCM16 -> one mu-law byte per sample.
    // Pending includes the host queue and BLE bytes not yet acknowledged.
    const pendingMs = Math.max(0, Math.round((response.pcmBytes / 2 - event.received) / 24))
    this.report({
      type: terminal ? event.state : 'underrun',
      sourceDone: response.sourceDone,
      sourceMs: Math.round(response.pcmBytes / 48),
      receivedMs: Math.round(event.received / 24),
      hostPendingMs: pendingMs,
      queuedMs: Number.isFinite(event.queuedMs) ? event.queuedMs : null,
      sourceGapMs: response.maxSourceGapMs,
      sourceAgeMs: response.lastDeltaAt === null ? null : Math.max(0, this.now() - response.lastDeltaAt),
      underruns: Math.max(0, event.underruns - response.firstUnderruns),
      dropped: Math.max(0, event.dropped - response.firstDropped),
    })
    if (terminal) this.responses.delete(event.responseId)
  }
}
