import assert from 'node:assert/strict'
import http from 'node:http'
import { once } from 'node:events'
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
