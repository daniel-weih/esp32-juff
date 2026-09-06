import { EventEmitter } from 'node:events'
import { GatewayClient } from './gateway-client.mjs'
import { isWakeAcknowledgementPlayback, wakeAcknowledgement } from './wake-acknowledgement.mjs'

// One device transport stays connected while its cloud conversation can close.
// Generation checks fence asynchronous connects, callbacks and old responses.
export class ConversationClient extends EventEmitter {
  constructor(options, { GatewayClientImpl = GatewayClient } = {}) {
    super()
    this.options = options
    this.GatewayClientImpl = GatewayClientImpl
    this.generation = null
    this.mode = 'continuous'
    this.active = true // Older firmware keeps its existing behavior.
    this.client = null
    this.starting = null
    this.closed = false
    this.retry = null
    this.pendingWakeAcknowledgement = false
  }

  setConversation(event) {
    if (this.closed || event?.type !== 'conversation.state'
      || !Number.isInteger(event.generation) || event.generation < 1 || event.generation > 0xffffffff
      || !['continuous', 'wake'].includes(event.mode) || typeof event.active !== 'boolean'
      || (event.mode === 'continuous' && !event.active)) return false
    if (this.generation !== null && event.generation !== this.generation
      && ((event.generation - this.generation) >>> 0) >= 0x80000000) return false
    const changed = event.generation !== this.generation
    if (!changed && (event.mode !== this.mode || event.active !== this.active)) return false
    if (changed) {
      this.retire()
      this.generation = event.generation
      this.mode = event.mode
      this.active = event.active
      this.pendingWakeAcknowledgement = this.mode === 'wake' && this.active
        && this.options.outputEnabled !== false
      this.emit('event', { type: 'playback.clear', reason: 'conversation boundary',
        conversationGeneration: this.generation })
    }
    this.emit('event', { type: 'conversation.ack', conversationGeneration: this.generation })
    if (this.active && !this.client) {
      void this.connect().catch(error => this.emit('error', error))
    }
    return true
  }

  async connect() {
    if (this.closed || !this.active) return null
    if (this.starting) return this.starting
    if (this.client?.connected) return this.client.health
    const client = new this.GatewayClientImpl(this.options)
    const generation = this.generation
    this.client = client
    const current = () => !this.closed && this.active && this.client === client
      && this.generation === generation
    client.on('event', event => {
      if (!current()) return
      // A real reply takes precedence if it arrives before voice readiness.
      if (event.type === 'response.started' || event.type === 'audio.delta') {
        this.pendingWakeAcknowledgement = false
      }
      this.emit('event', generation === null ? event
        : { ...event, conversationGeneration: generation })
      // The local WebSocket opening does not mean cloud audio input is ready.
      // Play the cached cue only after the real voice.ready, once per wake.
      if (current() && event.type === 'voice.ready' && this.pendingWakeAcknowledgement) {
        this.pendingWakeAcknowledgement = false
        for (const audio of wakeAcknowledgement(generation)) {
          if (!current()) break
          this.emit('event', audio)
        }
      }
    })
    client.on('error', error => { if (current()) this.emit('error', error) })
    client.on('close', (code, reason) => {
      if (!current()) return
      this.client = null
      this.emit('event', { type: 'voice.deactivated',
        ...(generation === null ? {} : { conversationGeneration: generation }) })
      this.emit('close', code, reason)
      this.scheduleReconnect()
    })
    const pending = (async () => {
      try {
        const health = await client.connect()
        if (!current()) { client.close(); return null }
        this.emit('ready', health)
        return health
      } catch (error) {
        if (!current()) return null
        this.client = null
        client.close()
        this.emit('error', error)
        this.scheduleReconnect()
        return null
      }
    })()
    this.starting = pending
    try { return await pending }
    finally { if (this.starting === pending) this.starting = null }
  }

  get connected() { return this.active && !this.closed && Boolean(this.client?.connected) }
  send(event) {
    // This response belongs to the host, not the cloud response lifecycle.
    // Consume late/duplicate local playback events after a mode boundary too.
    if (isWakeAcknowledgementPlayback(event)) return true
    if (event?.type === 'interrupt' || event?.type === 'sleep') {
      this.pendingWakeAcknowledgement = false
    }
    return this.connected ? this.client.send(event) : false
  }
  sendAudio(pcm) { return this.connected ? this.client.sendAudio(pcm) : false }

  retire() {
    this.pendingWakeAcknowledgement = false
    clearTimeout(this.retry)
    this.retry = null
    const client = this.client
    this.client = null
    this.starting = null
    client?.close()
  }

  scheduleReconnect() {
    if (this.closed || !this.active || this.retry) return
    this.retry = setTimeout(() => {
      this.retry = null
      void this.connect().catch(error => this.emit('error', error))
    }, 1500)
    this.retry.unref?.()
  }

  close() {
    this.closed = true
    this.retire()
  }
}
