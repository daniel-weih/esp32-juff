import assert from 'node:assert/strict'
import http from 'node:http'
import { EventEmitter, once } from 'node:events'
import test from 'node:test'
import WebSocket, { WebSocketServer } from 'ws'
import { createDeviceBridge } from '../src/device-bridge.mjs'

function messageQueue(socket) {
  const pending = []
  const waiters = []
  socket.on('message', (data, isBinary) => {
    const item = { data, isBinary }
    const waiter = waiters.shift()
    if (waiter) waiter(item)
    else pending.push(item)
  })
  return async function next() {
    if (pending.length) return pending.shift()
    return new Promise(resolve => waiters.push(resolve))
  }
}

async function fakeGateway() {
  const server = http.createServer((request, response) => {
    if (request.url === '/api/health') {
      response.writeHead(200, {
        'content-type': 'application/json',
        'set-cookie': 'qwaudio-test=owner; Path=/; HttpOnly',
      })
      response.end(JSON.stringify({
        backend: { protocol: 'none' },
        realtimeInputSampleRate: 16000,
        realtimeModel: 'test-realtime',
      }))
      return
    }
    response.writeHead(404).end()
  })
  const websocketServer = new WebSocketServer({ noServer: true })
  const connected = new Promise(resolve => websocketServer.once('connection', socket => {
    resolve({ socket, next: messageQueue(socket) })
  }))
  server.on('upgrade', (request, socket, head) => {
    websocketServer.handleUpgrade(request, socket, head, ws => websocketServer.emit('connection', ws, request))
  })
  server.listen(0, '127.0.0.1')
  await once(server, 'listening')
  return {
    url: `http://127.0.0.1:${server.address().port}`,
    connected,
    async close() {
      for (const client of websocketServer.clients) client.terminate()
      websocketServer.close()
      await new Promise(resolve => server.close(resolve))
    },
  }
}

test('bridges authenticated PCM and playback lifecycle events', async t => {
  const gateway = await fakeGateway()
  const bridge = createDeviceBridge({
    bridgeHost: '127.0.0.1',
    bridgePort: 0,
    devicePath: '/device',
    deviceToken: '0123456789abcdef',
    gatewayUrl: gateway.url,
    gatewaySessionId: 'test-device',
    gatewayTakeover: false,
    workingDirectory: process.cwd(),
  }, { logger: () => {} })
  await bridge.start()
  t.after(async () => {
    await bridge.close()
    await gateway.close()
  })

  const device = new WebSocket(`ws://127.0.0.1:${bridge.address.port}/device`)
  await once(device, 'open')
  const nextDevice = messageQueue(device)
  device.send(JSON.stringify({
    type: 'hello',
    token: '0123456789abcdef',
    deviceId: 'esp32-test',
    audioInputEnabled: true,
    audioOutputEnabled: true,
  }))

  assert.equal(JSON.parse((await nextDevice()).data).type, 'bridge.connecting')
  const { socket: gatewaySocket, next: nextGateway } = await gateway.connected
  const connect = JSON.parse((await nextGateway()).data)
  assert.equal(connect.type, 'connect')
  assert.equal(connect.clientLabel, 'ESP32 Audio Bridge')
  assert.equal(connect.inputEnabled, true)
  assert.equal(connect.outputEnabled, true)
  assert.deepEqual(connect.inputCapabilities, {
    text: true,
    audio: true,
    image: false,
    resource: false,
  })
  const ready = JSON.parse((await nextDevice()).data)
  assert.equal(ready.type, 'device.ready')
  assert.equal(ready.inputSampleRate, 16000)

  const mic = Buffer.from([0x01, 0x02, 0x03, 0x04])
  device.send(mic)
  const append = JSON.parse((await nextGateway()).data)
  assert.equal(append.type, 'audio.append')
  assert.deepEqual(Buffer.from(append.audio, 'base64'), mic)

  const speaker = Buffer.from([0x10, 0x20, 0x30, 0x40])
  gatewaySocket.send(JSON.stringify({
    type: 'audio.delta',
    responseId: 'response-1',
    sampleRate: 24000,
    audio: speaker.toString('base64'),
  }))
  const begin = await nextDevice()
  assert.equal(JSON.parse(begin.data).type, 'audio.begin')
  const audio = await nextDevice()
  assert.equal(audio.isBinary, true)
  assert.deepEqual(audio.data, speaker)

  device.send(JSON.stringify({ type: 'playback.started', responseId: 'response-1' }))
  const playback = JSON.parse((await nextGateway()).data)
  assert.equal(playback.type, 'playback.started')
  assert.equal(playback.responseId, 'response-1')
})

test('rejects an invalid device token', async t => {
  const gateway = await fakeGateway()
  const bridge = createDeviceBridge({
    bridgeHost: '127.0.0.1',
    bridgePort: 0,
    devicePath: '/device',
    deviceToken: '0123456789abcdef',
    gatewayUrl: gateway.url,
    gatewaySessionId: 'test-device',
    gatewayTakeover: false,
    workingDirectory: process.cwd(),
  }, { logger: () => {} })
  await bridge.start()
  t.after(async () => {
    await bridge.close()
    await gateway.close()
  })

  const device = new WebSocket(`ws://127.0.0.1:${bridge.address.port}/device`)
  await once(device, 'open')
  device.send(JSON.stringify({ type: 'hello', token: 'wrong-token-value' }))
  const [code] = await once(device, 'close')
  assert.equal(code, 1008)
})

test('Wi-Fi standby opens no cloud session and repeated mode handshakes preserve playback', { timeout: 10000 }, async t => {
  const clients = []
  class Voice extends EventEmitter {
    constructor() { super(); clients.push(this); this.connected = false; this.sent = [] }
    async connect() { this.connected = true; this.emit('event', { type: 'voice.ready' }); return { backend: {} } }
    close() { this.connected = false; this.closed = true; this.emit('close', 1000, '') }
    send(event) { this.sent.push(event); return true }
    sendAudio() { throw new Error('Standby audio reached the gateway') }
  }
  const bridge = createDeviceBridge({ bridgeHost: '127.0.0.1', bridgePort: 0,
    devicePath: '/device', deviceToken: 'synthetic-test-token', gatewayUrl: 'http://127.0.0.1',
    gatewaySessionId: 'fixture', gatewayTakeover: false, workingDirectory: process.cwd(),
  }, { logger: () => {}, GatewayClientImpl: Voice })
  await bridge.start()
  t.after(() => bridge.close())
  const device = new WebSocket(`ws://127.0.0.1:${bridge.address.port}/device`)
  await once(device, 'open')
  const next = messageQueue(device)
  const state = (generation, active) => ({ type: 'conversation.state', mode: 'wake', generation, active })
  device.send(JSON.stringify({ type: 'hello', token: 'synthetic-test-token', conversation: state(1, false) }))
  assert.equal(JSON.parse((await next()).data).type, 'bridge.connecting')
  assert.equal(JSON.parse((await next()).data).type, 'playback.clear')
  assert.equal(JSON.parse((await next()).data).type, 'conversation.ack')
  assert.equal(clients.length, 0)
  device.send(Buffer.from([1, 2]))
  device.send(JSON.stringify(state(2, true)))
  assert.equal(JSON.parse((await next()).data).type, 'playback.clear')
  assert.equal(JSON.parse((await next()).data).type, 'conversation.ack')
  assert.deepEqual(JSON.parse((await next()).data), { type: 'voice.ready', conversationGeneration: 2 })
  const greeting = JSON.parse((await next()).data)
  assert.equal(greeting.type, 'audio.begin')
  assert.equal(greeting.sampleRate, 24000)
  assert.equal(greeting.conversationGeneration, 2)
  let greetingBytes = 0
  while (true) {
    const frame = await next()
    if (frame.isBinary) greetingBytes += frame.data.length
    else {
      assert.deepEqual(JSON.parse(frame.data), { type: 'audio.done', responseId: greeting.responseId,
        conversationGeneration: 2 })
      break
    }
  }
  assert.ok(greetingBytes >= 12000 && greetingBytes <= 72000)
  device.send(JSON.stringify({ type: 'playback.started', responseId: greeting.responseId }))
  device.send(JSON.stringify({ type: 'playback.ended', responseId: greeting.responseId }))
  device.send(JSON.stringify(state(2, true)))
  assert.equal(JSON.parse((await next()).data).type, 'conversation.ack')
  assert.deepEqual(clients[0].sent, [], 'Local prompt playback must not reach cloud response tracking')
  const audio = { type: 'audio.delta', responseId: 'fixture', audio: Buffer.from([3, 4]).toString('base64') }
  clients[0].emit('event', audio)
  assert.equal(JSON.parse((await next()).data).conversationGeneration, 2)
  assert.equal((await next()).isBinary, true)
  device.send(JSON.stringify(state(2, true)))
  assert.equal(JSON.parse((await next()).data).type, 'conversation.ack')
  clients[0].emit('event', audio)
  assert.equal((await next()).isBinary, true, 'Duplicate handshake must not emit another audio.begin')
  device.send(JSON.stringify(state(3, false)))
  assert.equal(JSON.parse((await next()).data).type, 'playback.clear')
  assert.equal(JSON.parse((await next()).data).type, 'conversation.ack')
  assert.equal(clients[0].closed, true)
  clients[0].emit('event', audio)
  device.send(JSON.stringify(state(3, false)))
  assert.equal(JSON.parse((await next()).data).type, 'conversation.ack', 'Old cloud audio must be fenced')
})
