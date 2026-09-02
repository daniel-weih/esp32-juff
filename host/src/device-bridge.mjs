import { timingSafeEqual } from 'node:crypto'
import http from 'node:http'
import WebSocket, { WebSocketServer } from 'ws'
import { GatewayClient } from './gateway-client.mjs'

const FORWARDED_DEVICE_EVENTS = new Set([
  'interrupt',
  'mute',
  'unmute',
  'input.mute',
  'input.unmute',
  'input.suspend.ack',
  'sleep',
  'wake',
  'playback.started',
  'playback.ended',
  'playback.cancelled',
  'text.message',
])

function safeEqual(left, right) {
  const a = Buffer.from(String(left))
  const b = Buffer.from(String(right))
  return a.length === b.length && timingSafeEqual(a, b)
}

function sendJson(socket, event) {
  if (socket.readyState === WebSocket.OPEN) socket.send(JSON.stringify(event))
}

function defaultLogger(level, message, details = {}) {
  const record = { time: new Date().toISOString(), level, message, ...details }
  const line = `${JSON.stringify(record)}\n`
  if (level === 'error') process.stderr.write(line)
  else process.stdout.write(line)
}

export function createDeviceBridge(config, {
  logger = defaultLogger,
  GatewayClientImpl = GatewayClient,
} = {}) {
  let active = null
  const server = http.createServer((request, response) => {
    if (request.url === '/healthz') {
      response.writeHead(200, { 'content-type': 'application/json' })
      response.end(JSON.stringify({ ok: true, deviceConnected: Boolean(active?.authenticated) }))
      return
    }
    response.writeHead(404, { 'content-type': 'application/json' })
    response.end(JSON.stringify({ error: 'not found' }))
  })
  const websocketServer = new WebSocketServer({ noServer: true, maxPayload: 64 * 1024 })

  function release(session) {
    clearTimeout(session.helloTimer)
    session.gateway?.close()
    if (active === session) active = null
  }

  function attachDevice(socket, request) {
    const session = {
      socket,
      request,
      authenticated: false,
      gateway: null,
      activeResponseId: '',
      helloTimer: null,
    }

    session.helloTimer = setTimeout(() => {
      if (!session.authenticated) socket.close(1008, 'hello timeout')
    }, 5000)

    const handleGatewayEvent = event => {
      if (socket.readyState !== WebSocket.OPEN) return
      if (event.type === 'audio.delta') {
        const responseId = String(event.responseId || '')
        if (!responseId) return
        if (session.activeResponseId !== responseId) {
          session.activeResponseId = responseId
          sendJson(socket, {
            type: 'audio.begin',
            responseId,
            sampleRate: Number(event.sampleRate) || 24000,
          })
        }
        socket.send(Buffer.from(event.audio || '', 'base64'), { binary: true })
        return
      }
      if (event.type === 'audio.done') {
        sendJson(socket, event)
        return
      }
      if (event.type === 'playback.clear') session.activeResponseId = ''
      sendJson(socket, event)
    }

    async function authenticate(message) {
      if (message?.type !== 'hello' || !safeEqual(message.token, config.deviceToken)) {
        socket.close(1008, 'authentication failed')
        return
      }
      if (active && active !== session) {
        socket.close(1013, 'another device owns the voice session')
        return
      }

      clearTimeout(session.helloTimer)
      session.authenticated = true
      active = session
      sendJson(socket, { type: 'bridge.connecting' })

      const gateway = new GatewayClientImpl({
        baseUrl: config.gatewayUrl,
        sessionId: config.gatewaySessionId,
        takeover: config.gatewayTakeover,
        inputEnabled: message.audioInputEnabled !== false,
        outputEnabled: message.audioOutputEnabled !== false,
        workingDirectory: config.workingDirectory,
      })
      session.gateway = gateway
      gateway.on('event', handleGatewayEvent)
      gateway.on('error', error => {
        logger('error', 'gateway websocket error', { error: error.message })
      })
      gateway.on('close', (code, reason) => {
        if (socket.readyState === WebSocket.OPEN) socket.close(1011, `gateway closed: ${code} ${reason}`)
      })

      try {
        const health = await gateway.connect()
        sendJson(socket, {
          type: 'device.ready',
          deviceId: String(message.deviceId || 'unknown'),
          inputSampleRate: Number(health.realtimeInputSampleRate) || 16000,
          realtimeModel: health.realtimeModel || health.realtimeModelProfile?.id || '',
        })
        logger('info', 'device authenticated', {
          deviceId: String(message.deviceId || 'unknown'),
          remote: request.socket.remoteAddress,
        })
      } catch (error) {
        logger('error', 'gateway connection failed', { error: error.message })
        sendJson(socket, { type: 'error', message: error.message })
        socket.close(1011, 'gateway unavailable')
      }
    }

    socket.on('message', (data, isBinary) => {
      if (!session.authenticated) {
        if (isBinary) {
          socket.close(1008, 'hello required')
          return
        }
        let message
        try {
          message = JSON.parse(data.toString())
        } catch {
          socket.close(1007, 'invalid hello')
          return
        }
        void authenticate(message)
        return
      }

      if (isBinary) {
        session.gateway?.sendAudio(data)
        return
      }

      let message
      try {
        message = JSON.parse(data.toString())
      } catch {
        return
      }
      if (!FORWARDED_DEVICE_EVENTS.has(message.type)) return
      if (message.type === 'playback.ended' || message.type === 'playback.cancelled') {
        if (message.responseId === session.activeResponseId) session.activeResponseId = ''
      }
      session.gateway?.send(message)
    })
    socket.on('error', error => logger('error', 'device websocket error', { error: error.message }))
    socket.on('close', () => release(session))
  }

  server.on('upgrade', (request, socket, head) => {
    const url = new URL(request.url || '/', 'http://bridge.local')
    if (url.pathname !== config.devicePath) {
      socket.write('HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n')
      socket.destroy()
      return
    }
    websocketServer.handleUpgrade(request, socket, head, client => attachDevice(client, request))
  })

  return {
    async start() {
      await new Promise((resolve, reject) => {
        server.once('error', reject)
        server.listen(config.bridgePort, config.bridgeHost, () => {
          server.off('error', reject)
          resolve()
        })
      })
      const address = server.address()
      logger('info', 'device bridge listening', {
        host: typeof address === 'object' ? address.address : config.bridgeHost,
        port: typeof address === 'object' ? address.port : config.bridgePort,
        path: config.devicePath,
      })
      return address
    },
    async close() {
      if (active?.socket.readyState === WebSocket.OPEN) active.socket.close(1001, 'bridge stopping')
      active?.gateway?.close()
      active = null
      websocketServer.close()
      await new Promise(resolve => server.close(() => resolve()))
    },
    get address() {
      return server.address()
    },
  }
}
