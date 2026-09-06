import { EventEmitter } from 'node:events'
import WebSocket from 'ws'

function websocketUrl(baseUrl, sessionId) {
  const url = new URL('/api/realtime', baseUrl)
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:'
  url.searchParams.set('sessionId', sessionId)
  return url.toString()
}

function responseCookie(response) {
  const raw = response.headers.getSetCookie?.()[0]
    || response.headers.get('set-cookie')
    || ''
  return raw.split(';', 1)[0]
}

export class GatewayClient extends EventEmitter {
  constructor({
    baseUrl,
    sessionId,
    takeover = false,
    inputEnabled = true,
    outputEnabled = true,
    workingDirectory = process.cwd(),
    fetchImpl = fetch,
    WebSocketImpl = WebSocket,
  }) {
    super()
    this.baseUrl = baseUrl
    this.sessionId = sessionId
    this.takeover = takeover
    this.inputEnabled = inputEnabled
    this.outputEnabled = outputEnabled
    this.workingDirectory = workingDirectory
    this.fetchImpl = fetchImpl
    this.WebSocketImpl = WebSocketImpl
    this.socket = null
    this.health = null
    this.closed = false
    this.abort = new AbortController()
    this.voiceConfigured = false
    this.waitingForVoice = false
    this.voiceRecoveryEnabled = true
    this.inputMuted = false
  }

  async connect() {
    if (this.closed) throw new Error('Gateway client is closed')
    const response = await this.fetchImpl(`${this.baseUrl}/api/health`, {
      signal: AbortSignal.any([this.abort.signal, AbortSignal.timeout(5000)]),
    })
    const health = await response.json().catch(() => null)
    if (!response.ok || !health || typeof health !== 'object' || !health.backend) {
      throw new Error(health?.error || health?.backend?.error || 'qwen-audio-agent Gateway is not ready')
    }
    this.health = health
    if (this.closed) throw new Error('Gateway client is closed')

    const cookie = responseCookie(response)
    const headers = cookie ? { Cookie: cookie } : {}
    const socket = new this.WebSocketImpl(websocketUrl(this.baseUrl, this.sessionId), { headers })
    this.socket = socket

    socket.on('message', raw => {
      if (this.closed || this.socket !== socket) return
      let event
      try {
        event = JSON.parse(raw.toString())
      } catch {
        return
      }
      const ownership = event?.type === 'voice.ownership' ? event.state : null
      if (ownership === 'busy' || event?.type === 'voice.deactivated') this.waitingForVoice = true
      else if (ownership === 'active') this.waitingForVoice = false
      this.emit('event', event)
      // Qwen keeps a displaced/denied client's socket open. A free owner slot
      // therefore needs a new CONNECT, not a WebSocket reconnection. Never
      // take over during recovery: competing waiters must not evict each other.
      if (ownership === 'available' && this.waitingForVoice && this.voiceConfigured
        && this.voiceRecoveryEnabled && this.outputEnabled && !this.closed && this.socket === socket) {
        this.activateVoice({ takeover: false })
      }
    })
    socket.on('error', error => this.emit('error', error))
    socket.on('close', (code, reason) => {
      if (this.socket === socket) this.socket = null
      this.emit('close', code, reason.toString())
    })

    await new Promise((resolve, reject) => {
      const onOpen = () => {
        cleanup()
        resolve()
      }
      const onError = error => {
        cleanup()
        reject(error)
      }
      const onClose = () => { cleanup(); reject(new Error('Gateway closed before connecting')) }
      const cleanup = () => {
        socket.off('open', onOpen)
        socket.off('error', onError)
        socket.off('close', onClose)
      }
      socket.once('open', onOpen)
      socket.once('error', onError)
      socket.once('close', onClose)
    })

    this.activateVoice({ takeover: this.takeover })
    return health
  }

  activateVoice({ takeover = false } = {}) {
    if (this.closed || !this.connected) return false
    // Consume availability before sending; duplicate notices cannot enqueue
    // multiple claims while the first request is still awaiting arbitration.
    this.voiceConfigured = true
    this.waitingForVoice = false
    return this.send({
      type: 'connect',
      voiceEnabled: this.inputEnabled || this.outputEnabled,
      inputEnabled: this.inputEnabled && !this.inputMuted,
      outputEnabled: this.outputEnabled,
      clientType: 'cli',
      clientLabel: 'ESP32 Audio Bridge',
      inputCapabilities: {
        text: true,
        audio: true,
        image: false,
        resource: false,
      },
      takeover,
      workingDirectory: this.workingDirectory,
      timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone,
      locale: Intl.DateTimeFormat().resolvedOptions().locale,
    })
  }

  get connected() {
    return this.socket?.readyState === this.WebSocketImpl.OPEN
  }

  send(event) {
    if (!this.connected) return false
    if (event?.type === 'mute' || event?.type === 'sleep') {
      this.voiceRecoveryEnabled = false
      this.waitingForVoice = false
    } else if (['unmute', 'input.unmute', 'wake'].includes(event?.type)) {
      this.voiceRecoveryEnabled = true
    }
    if (event?.type === 'input.mute') this.inputMuted = true
    else if (event?.type === 'unmute' || event?.type === 'input.unmute') this.inputMuted = false
    this.socket.send(JSON.stringify(event))
    return true
  }

  sendAudio(pcm) {
    return this.send({
      type: 'audio.append',
      audio: Buffer.from(pcm).toString('base64'),
    })
  }

  close() {
    this.closed = true
    this.waitingForVoice = false
    this.voiceConfigured = false
    this.abort.abort()
    const socket = this.socket
    this.socket = null
    if (socket?.readyState < this.WebSocketImpl.CLOSING) socket.close(1000, 'device disconnected')
  }
}
