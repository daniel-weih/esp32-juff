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
  }

  async connect() {
    const response = await this.fetchImpl(`${this.baseUrl}/api/health`, {
      signal: AbortSignal.timeout(5000),
    })
    const health = await response.json().catch(() => null)
    if (!response.ok || !health || typeof health !== 'object' || !health.backend) {
      throw new Error(health?.error || health?.backend?.error || 'qwen-audio-agent Gateway is not ready')
    }
    this.health = health

    const cookie = responseCookie(response)
    const headers = cookie ? { Cookie: cookie } : {}
    const socket = new this.WebSocketImpl(websocketUrl(this.baseUrl, this.sessionId), { headers })
    this.socket = socket

    socket.on('message', raw => {
      let event
      try {
        event = JSON.parse(raw.toString())
      } catch {
        return
      }
      this.emit('event', event)
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
      const cleanup = () => {
        socket.off('open', onOpen)
        socket.off('error', onError)
      }
      socket.once('open', onOpen)
      socket.once('error', onError)
    })

    this.send({
      type: 'connect',
      voiceEnabled: this.inputEnabled || this.outputEnabled,
      inputEnabled: this.inputEnabled,
      outputEnabled: this.outputEnabled,
      clientType: 'cli',
      clientLabel: 'ESP32 Audio Bridge',
      inputCapabilities: {
        text: true,
        audio: true,
        image: false,
        resource: false,
      },
      takeover: this.takeover,
      workingDirectory: this.workingDirectory,
      timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone,
      locale: Intl.DateTimeFormat().resolvedOptions().locale,
    })

    return health
  }

  get connected() {
    return this.socket?.readyState === this.WebSocketImpl.OPEN
  }

  send(event) {
    if (!this.connected) return false
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
    const socket = this.socket
    this.socket = null
    if (socket?.readyState < this.WebSocketImpl.CLOSING) socket.close(1000, 'device disconnected')
  }
}
